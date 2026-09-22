// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_MERLIN_TEST_CLONE_TRACKING_EVENT_H
#define SST_ELEMENTS_MERLIN_TEST_CLONE_TRACKING_EVENT_H

#include <sst/core/event.h>

#include <stdexcept>

namespace SST::Merlin::Test {

/** Native payload that counts clones and destructions and can fail to clone. */
class CloneTrackingEvent final : public SST::Event
{
public:
    CloneTrackingEvent(int& clones, int& destructions, bool return_null = false, bool throw_on_clone = false) :
        clones_(&clones),
        destructions_(&destructions),
        return_null_(return_null),
        throw_on_clone_(throw_on_clone)
    {}

    ~CloneTrackingEvent() override { ++*destructions_; }

    SST::Event* clone() override
    {
        ++*clones_;
        if ( throw_on_clone_ ) throw std::runtime_error("test clone failure");
        if ( return_null_ ) return nullptr;
        return new CloneTrackingEvent(*clones_, *destructions_);
    }

private:
    int* clones_;
    int* destructions_;
    bool return_null_;
    bool throw_on_clone_;
};

} // namespace SST::Merlin::Test

#endif // SST_ELEMENTS_MERLIN_TEST_CLONE_TRACKING_EVENT_H
