/*
 * Copyright (c) 2016 CartoDB. All rights reserved.
 * Copying and using this code is allowed only according
 * to license terms, as given in https://cartodb.com/terms/
 */

#ifndef _CARTO_VT_LABELCULLER_H_
#define _CARTO_VT_LABELCULLER_H_

#include "ViewState.h"
#include "Label.h"

#include <array>
#include <vector>
#include <list>
#include <unordered_map>
#include <memory>
#include <mutex>

#include <cglib/vec.h>
#include <cglib/mat.h>
#include <cglib/bbox.h>

namespace carto { namespace vt {
    class LabelCuller final {
    public:
        explicit LabelCuller(std::shared_ptr<const TileTransformer> transformer, float scale);

        void setViewState(const ViewState& viewState);
        void process(const std::vector<std::shared_ptr<Label>>& labelList, std::mutex& labelMutex);

    private:
        constexpr static int GRID_RESOLUTION = 16;

        struct Record {
            cglib::bbox2<float> bounds;
            std::array<cglib::vec2<float>, 4> envelope;
            std::shared_ptr<Label> label;

            Record() = default;
            explicit Record(const cglib::bbox2<float>& bounds, const std::array<cglib::vec2<float>, 4>& envelope, std::shared_ptr<Label> label) : bounds(bounds), envelope(envelope), label(std::move(label)) { }
        };

        void clearGrid();
        bool testOverlap(const std::shared_ptr<Label>& label);

        static int getGridIndex(float x);

        cglib::mat4x4<float> _localCameraProjMatrix;
        ViewState _viewState;
        std::vector<Record> _recordGrid[GRID_RESOLUTION][GRID_RESOLUTION];

        const std::shared_ptr<const TileTransformer> _transformer;
        const float _scale;

        mutable std::mutex _mutex;
    };
} }

#endif
