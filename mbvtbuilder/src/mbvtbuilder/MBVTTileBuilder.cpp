#include "MBVTTileBuilder.h"
#include "MBVTLayerEncoder.h"
#include "Clipper.h"
#include "Simplifier.h"

#include <functional>
#include <algorithm>
#include <stdexcept>
#include <cmath>

#include "mbvtpackage/MBVTPackage.pb.h"

namespace carto { namespace mbvtbuilder {
    MBVTTileBuilder::MBVTTileBuilder(int minZoom, int maxZoom) :
        _minZoom(minZoom), _maxZoom(maxZoom)
    {
        createLayer("");
    }

    void MBVTTileBuilder::createLayer(const std::string& layerId, float buffer) {
        Layer layer;
        layer.layerId = layerId;
        layer.buffer = (buffer >= 0 ? buffer : DEFAULT_LAYER_BUFFER);
        _layers.push_back(std::move(layer));
    }

    void MBVTTileBuilder::addMultiPoint(MultiPoint coords, picojson::value properties) {
        Bounds bounds = Bounds::make_union(coords.begin(), coords.end());

        Feature feature;
        feature.bounds = bounds;
        feature.geometry = std::move(coords);
        feature.properties = std::move(properties);
        
        _layers.back().bounds.add(feature.bounds);
        _layers.back().features.push_back(std::move(feature));
    }

    void MBVTTileBuilder::addMultiLineString(MultiLineString coordsList, picojson::value properties) {
        Bounds bounds = Bounds::smallest();
        for (const std::vector<Point>& coords : coordsList) {
            bounds.add(Bounds::make_union(coords.begin(), coords.end()));
        }

        Feature feature;
        feature.bounds = bounds;
        feature.geometry = std::move(coordsList);
        feature.properties = std::move(properties);
        
        _layers.back().bounds.add(feature.bounds);
        _layers.back().features.push_back(std::move(feature));
    }

    void MBVTTileBuilder::addMultiPolygon(MultiPolygon ringsList, picojson::value properties) {
        Bounds bounds = Bounds::smallest();
        for (const std::vector<std::vector<Point>>& rings : ringsList) {
            for (const std::vector<Point>& ring : rings) {
                bounds.add(Bounds::make_union(ring.begin(), ring.end()));
            }
        }

        Feature feature;
        feature.bounds = bounds;
        feature.geometry = std::move(ringsList);
        feature.properties = std::move(properties);
        
        _layers.back().bounds.add(feature.bounds);
        _layers.back().features.push_back(std::move(feature));
    }

    void MBVTTileBuilder::importGeoJSON(const picojson::value& geoJSON) {
        std::string type = geoJSON.get("type").get<std::string>();
        if (type == "FeatureCollection") {
            importGeoJSONFeatureCollection(geoJSON);
        } else if (type == "Feature") {
            importGeoJSONFeature(geoJSON);
        } else {
            throw std::runtime_error("Unexpected element type");
        }
    }

    void MBVTTileBuilder::importGeoJSONFeatureCollection(const picojson::value& featureCollectionDef) {
        const picojson::array& featuresDef = featureCollectionDef.get("features").get<picojson::array>();

        for (const picojson::value& featureDef : featuresDef) {
            std::string type = featureDef.get("type").get<std::string>();
            if (type != "Feature") {
                throw std::runtime_error("Unexpected element type");
            }
            
            importGeoJSONFeature(featureDef);
        }
    }

    void MBVTTileBuilder::importGeoJSONFeature(const picojson::value& featureDef) {
        const picojson::value& geometryDef = featureDef.get("geometry");
        const picojson::value& properties = featureDef.get("properties");

        std::string type = geometryDef.get("type").get<std::string>();
        const picojson::value& coordsDef = geometryDef.get("coordinates");

        if (type == "Point") {
            addMultiPoint({ parseCoordinates(coordsDef) }, properties);
        }
        else if (type == "LineString") {
            addMultiLineString({ parseCoordinatesList(coordsDef) }, properties);
        }
        else if (type == "Polygon") {
            addMultiPolygon({ parseCoordinatesRings(coordsDef) }, properties);
        }
        else if (type == "MultiPoint") {
            std::vector<cglib::vec2<double>> coords;
            for (const picojson::value& subCoordsDef : coordsDef.get<picojson::array>()) {
                coords.push_back(parseCoordinates(subCoordsDef));
            }
            addMultiPoint(coords, properties);
        }
        else if (type == "MultiLineString") {
            std::vector<std::vector<cglib::vec2<double>>> coordsList;
            for (const picojson::value& subCoordsDef : coordsDef.get<picojson::array>()) {
                coordsList.push_back(parseCoordinatesList(subCoordsDef));
            }
            addMultiLineString(coordsList, properties);
        }
        else if (type == "MultiPolygon") {
            std::vector<std::vector<std::vector<cglib::vec2<double>>>> ringsList;
            for (const picojson::value& subCoordsDef : coordsDef.get<picojson::array>()) {
                ringsList.push_back(parseCoordinatesRings(subCoordsDef));
            }
            addMultiPolygon(ringsList, properties);
        }
        else {
            throw std::runtime_error("Invalid geometry type");
        }
    }

    void MBVTTileBuilder::buildTiles(std::function<void(int, int, int, const protobuf::encoded_message&)> handler) const {
        static constexpr Bounds mapBounds(Point(-PI * EARTH_RADIUS, -PI * EARTH_RADIUS), Point(PI * EARTH_RADIUS, PI * EARTH_RADIUS));
        
        std::vector<Layer> layers = _layers;
        for (int zoom = _maxZoom; zoom >= _minZoom; zoom--) {
            Bounds bounds = Bounds::smallest(); 
            for (Layer& layer : layers) {
                double tolerance = 2.0 * PI * EARTH_RADIUS / (1 << zoom) * TILE_TOLERANCE;
                simplifyLayer(layer, tolerance);

                cglib::vec2<double> layerBuffer(layer.buffer, layer.buffer);
                for (const Feature& feature : layer.features) {
                    bounds.add(feature.bounds.min - cglib::vec2<double>(layer.buffer, layer.buffer));
                    bounds.add(feature.bounds.max + cglib::vec2<double>(layer.buffer, layer.buffer));
                }
            }

            double tileSize = (mapBounds.max(0) - mapBounds.min(0)) / (1 << zoom);
            double tileCount = (1 << zoom);

            double tileX0 = std::max(0.0, std::floor((bounds.min(0) - mapBounds.min(0)) / tileSize));
            double tileY0 = std::max(0.0, std::floor((bounds.min(1) - mapBounds.min(1)) / tileSize));
            double tileX1 = std::min(tileCount, std::floor((bounds.max(0) - mapBounds.min(0)) / tileSize) + 1);
            double tileY1 = std::min(tileCount, std::floor((bounds.max(1) - mapBounds.min(1)) / tileSize) + 1);

            for (int tileY = static_cast<int>(tileY0); tileY < tileY1; tileY++) {
                for (int tileX = static_cast<int>(tileX0); tileX < tileX1; tileX++) {
                    protobuf::encoded_message encodedTile;
                    for (const Layer& layer : layers) {
                        if (layer.features.empty()) {
                            continue;
                        }

                        Point tileOrigin(tileX * tileSize + mapBounds.min(0), tileY * tileSize + mapBounds.min(1));
                        Bounds tileBounds(tileOrigin - cglib::vec2<double>(layer.buffer, layer.buffer) * tileSize, tileOrigin + cglib::vec2<double>(1 + layer.buffer, 1 + layer.buffer) * tileSize);

                        MBVTLayerEncoder layerEncoder(layer.layerId);
                        if (encodeLayer(layer, tileOrigin, tileSize, tileBounds, layerEncoder)) {
                            encodedTile.write_tag(vector_tile::Tile::kLayersFieldNumber);
                            encodedTile.write_message(layerEncoder.buildLayer());
                        }
                    }

                    if (!encodedTile.empty()) {
                        handler(zoom, tileX, tileY, encodedTile);
                    }
                }
            }
        }
    }

    void MBVTTileBuilder::simplifyLayer(Layer& layer, double tolerance) {
        struct GeometryVisitor : boost::static_visitor<> {
            explicit GeometryVisitor(double tolerance) : _simplifier(tolerance) { }

            void operator() (MultiPoint& coords) {
            }

            void operator() (MultiLineString& coordsList) {
                for (std::vector<Point>& coords : coordsList) {
                    coords = _simplifier.simplifyLineString(coords);
                }
            }

            void operator() (MultiPolygon& ringsList) {
                for (std::vector<std::vector<Point>>& rings : ringsList) {
                    for (auto it = rings.begin(); it != rings.end(); ) {
                        std::vector<Point> simplifiedRing = _simplifier.simplifyPolygonRing(*it);
                        if (simplifiedRing.size() >= 3) {
                            *it++ = std::move(simplifiedRing);
                        } else {
                            it = rings.erase(it);
                        }
                    }
                }
            }

        private:
            const Simplifier<double> _simplifier;
        };

        for (Feature& feature : layer.features) {
            GeometryVisitor visitor(tolerance);
            boost::apply_visitor(visitor, feature.geometry);
        }
    }

    bool MBVTTileBuilder::encodeLayer(const Layer& layer, const Point& tileOrigin, double tileSize, const Bounds& tileBounds, MBVTLayerEncoder& layerEncoder) {
        struct GeometryVisitor : boost::static_visitor<bool> {
            explicit GeometryVisitor(const Point& tileOrigin, double tileSize, const Bounds& tileBounds, const picojson::value& properties, MBVTLayerEncoder& layerEncoder) : _tileOrigin(tileOrigin), _tileScale(1.0 / tileSize), _clipper(tileBounds), _properties(properties), _layerEncoder(layerEncoder) { }

            bool operator() (const MultiPoint& coords) {
                std::vector<MBVTLayerEncoder::Point> tileCoords;
                tileCoords.reserve(coords.size());
                for (const Point& pos : coords) {
                    if (_clipper.testPoint(pos)) {
                        tileCoords.push_back(MBVTLayerEncoder::Point::convert((pos - _tileOrigin) * _tileScale));
                    }
                }
                _layerEncoder.addMultiPoint(tileCoords, _properties);
                return !tileCoords.empty();
            }

            bool operator() (const MultiLineString& coordsList) {
                std::vector<std::vector<MBVTLayerEncoder::Point>> tileCoordsList;
                tileCoordsList.reserve(coordsList.size());
                for (const std::vector<Point>& coords : coordsList) {
                    std::vector<std::vector<Point>> clippedCoordsList = _clipper.clipLineString(coords);
                    for (const std::vector<Point>& clippedCoords : clippedCoordsList) {
                        std::vector<MBVTLayerEncoder::Point> tileCoords;
                        tileCoords.reserve(clippedCoords.size());
                        for (const Point& pos : clippedCoords) {
                            tileCoords.push_back(MBVTLayerEncoder::Point::convert((pos - _tileOrigin) * _tileScale));
                        }
                        tileCoordsList.push_back(std::move(tileCoords));
                    }
                }
                _layerEncoder.addMultiLineString(tileCoordsList, _properties);
                return !tileCoordsList.empty();
            }

            bool operator() (const MultiPolygon& ringsList) {
                std::vector<std::vector<MBVTLayerEncoder::Point>> tileCoordsList;
                for (const std::vector<std::vector<Point>>& rings : ringsList) {
                    for (std::size_t i = 0; i < rings.size(); i++) {
                        std::vector<Point> clippedRing = _clipper.clipPolygonRing(rings[i]);
                        if (clippedRing.size() < 3) {
                            continue;
                        }

                        std::vector<MBVTLayerEncoder::Point> tileCoords;
                        tileCoords.reserve(clippedRing.size());
                        double signedArea = 0;
                        Point prevPos = clippedRing.back();
                        for (const Point& pos : clippedRing) {
                            tileCoords.push_back(MBVTLayerEncoder::Point::convert((pos - _tileOrigin) * _tileScale));
                            signedArea += (prevPos(0) - _tileOrigin(0)) * (pos(1) - _tileOrigin(1)) - (prevPos(1) - _tileOrigin(1)) * (pos(0) - _tileOrigin(0));
                            prevPos = pos;
                        }
                        if ((signedArea < 0) != (i == 0)) {
                            std::reverse(tileCoords.begin(), tileCoords.end());
                        }
                        tileCoordsList.push_back(std::move(tileCoords));
                    }
                }
                _layerEncoder.addMultiPolygon(tileCoordsList, _properties);
                return !tileCoordsList.empty();
            }

        private:
            const Point _tileOrigin;
            const double _tileScale;
            const Clipper<double> _clipper;
            const picojson::value& _properties;
            MBVTLayerEncoder& _layerEncoder;
        };

        bool featuresAdded = false;
        for (const Feature& feature : layer.features) {
            if (!tileBounds.inside(feature.bounds)) {
                continue;
            }

            GeometryVisitor visitor(tileOrigin, tileSize, tileBounds, feature.properties, layerEncoder);
            if (boost::apply_visitor(visitor, feature.geometry)) {
                featuresAdded = true;
            }
        }

        return featuresAdded;
    }

    std::vector<std::vector<MBVTTileBuilder::Point>> MBVTTileBuilder::parseCoordinatesRings(const picojson::value& coordsDef) {
        const picojson::array& coordsArray = coordsDef.get<picojson::array>();

        std::vector<std::vector<Point>> rings;
        rings.reserve(coordsArray.size());
        for (std::size_t i = 0; i < coordsArray.size(); i++) {
            std::vector<Point> coordsList = parseCoordinatesList(coordsArray[i]);
            rings.push_back(std::move(coordsList));
        }
        return rings;
    }

    std::vector<MBVTTileBuilder::Point> MBVTTileBuilder::parseCoordinatesList(const picojson::value& coordsDef) {
        const picojson::array& coordsArray = coordsDef.get<picojson::array>();

        std::vector<Point> coordsList;
        coordsList.reserve(coordsArray.size());
        for (std::size_t i = 0; i < coordsArray.size(); i++) {
            Point coords = parseCoordinates(coordsArray[i]);
            coordsList.push_back(coords);
        }
        return coordsList;
    }

    MBVTTileBuilder::Point MBVTTileBuilder::parseCoordinates(const picojson::value& coordsDef) {
        const picojson::array& coordsArray = coordsDef.get<picojson::array>();
        cglib::vec2<double> posWgs84(coordsArray.at(0).get<double>(), coordsArray.at(1).get<double>());
        return wgs84ToWM(posWgs84);
    }

    MBVTTileBuilder::Point MBVTTileBuilder::wgs84ToWM(const cglib::vec2<double>& posWgs84) {
        double x = EARTH_RADIUS * posWgs84(0) * PI / 180.0;
        double a = posWgs84(1) * PI / 180.0;
        double y = 0.5 * EARTH_RADIUS * std::log((1.0 + std::sin(a)) / (1.0 - std::sin(a)));
        return Point(x, y);
    }
} }