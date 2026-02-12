// Copyright 2009-2024 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2024, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


// This include is ***REQUIRED***
// for ALL SST implementation files
#include "sst_config.h"

#include "sst/elements/carcosa/Components/SensorEvent.h"
#include "sst/elements/carcosa/Components/SensorComponent.h"


using namespace SST;
using namespace SST::Carcosa;

/*
 * Lifecycle Phase #1: Construction
 * - Configure output object
 * - Ensure iflLink is connected and configure
 * - Configure the clock to regularly send messages to IFL
 * - Read parameters
 * - Initialize internal state
 */
SensorComponent::SensorComponent(ComponentId_t id, Params& params) : Component(id)
{
    requireLibrary("memHierarchy");

    // SST Output Object
    // Initialize with
    // - no prefix ("")
    // - Verbose set to 1
    // - No mask
    // - Output to STDOUT (Output::STDOUT)
    out = new Output("", 1, 0, Output::STDOUT);

    // Get parameter from the Python input
    bool found;
    eventsToSend = params.find<unsigned>("eventsToSend", 0, found);

    // If parameter wasn't found, end the simulation with exit code -1.
    // Tell the user how to fix the error (set 'eventsToSend' parameter in the input)
    // and which component generated the error (getName())
    if (!found) {
        out->fatal(CALL_INFO, -1, "Error in %s: the input did not specify 'eventsToSend' parameter\n", getName().c_str());
    }

    // Verbose parameter to control output further
    verbose = params.find<bool>("verbose", false);

    // configure our links with a callback function that will be called whenever an event arrives
    iflLink = configureLink("ifl", new Event::Handler2<SensorComponent, &SensorComponent::handleSensorEvent>(this));

    registerClock("1Hz", new Clock::Handler2<SensorComponent, &SensorComponent::mainTick>(this));
    // Make sure we successfully configured the links
    // Failure usually means the user didn't connect the port in the input file
    sst_assert(iflLink, CALL_INFO, -1, "Error in %s: IFL link configuration failed\n", getName().c_str());

    // Register as primary and prevent simulation end until we've received all the events we need
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    // Initialize our variables that count events
    eventsReceived = 0;
    eventsForwarded = 0;
    eventsSent = 0;
}

/*
 * Lifecycle Phase #2: Init
 * SensorComponent initiates communication with the Interface
 * Layer (IFL) and establishes the number of events to send
 * - Send our eventsToSend parameter to the IFL
 * - During init(), the 'sendUntimedData' functions must be used instead of 'send'
 */
void SensorComponent::init(unsigned phase)
{
    // Only send our info on phase 0
    if (phase == 0) {
        SensorEvent* event = new SensorEvent(getName());
        iflLink->sendUntimedData(event);
    }

    // Check if an event is received. recvUntimedData returns nullptr if no event is available
    while (SST::Event* ev = iflLink->recvUntimedData()) {

        SensorEvent* event = dynamic_cast<SensorEvent*>(ev);
        if (event) {
                if (verbose) out->output("    %" PRIu64 " %s received %s\n", getCurrentSimCycle(), getName().c_str(), event->toString().c_str());
                delete event;
        } else {
            out->fatal(CALL_INFO, -1, "Error in %s: Received an event during init() but it is not the expected type\n", getName().c_str());
        }
    }
}

/*
 * Lifecycle Phase #3: Setup
 * - Send first event
 *   This send should use the link->send function since it is intended for simulation rather than
 *   pre-simulation (init) or post-simulation (complete)
 * - Initialize the 'iter' variable to point to next component to send to in eventRequests map
 */
void SensorComponent::setup()
{
    // Total events to send during simulation
    eventsToSend = 10;

    // Sanity check
    if (iflLink == NULL) {
        primaryComponentOKToEndSim();
        return;
    } else if (eventsToSend == 0) {
        primaryComponentOKToEndSim();
        return;
    }

    // Send first event
    iflLink->send(new SensorEvent("hi"));
    eventsSent++;
}


/*
 * Lifecycle Phase #4: Run
 *
 * During the run phase, SST will call this event handler anytime an
 * event is received on a link
 * - Really shouldn't have messages going this direction
 */
void SensorComponent::handleSensorEvent(SST::Event *ev)
{
    SensorEvent *event = dynamic_cast<SensorEvent*>(ev);

    if (event) {
        if (verbose) out->output("    %" PRIu64 " %s received %s\n", getCurrentSimCycle(), getName().c_str(), event->toString().c_str());
    } else {
        out->fatal(CALL_INFO, -1, "Error in %s: Received an event during simulation but it is not the expected type\n", getName().c_str());
    }
}

/*
 * Lifecycle Phase #5: Complete
 *
 * During the complete phase, say goodbye to our left neighbor and farewell to our right
 */
void SensorComponent::complete(unsigned phase)
{
    if (phase == 0) {
        std::string goodbye = "Goodbye from " + getName();
        iflLink->sendUntimedData( new SensorEvent(goodbye) );
    }

    // Check for an event on the ifl link
    while (SST::Event* ev = iflLink->recvUntimedData()) {
        SensorEvent* event = dynamic_cast<SensorEvent*>(ev);
        if (event) {
            if (verbose) out->output("    %" PRIu64 " %s received %s\n", getCurrentSimCycle(), getName().c_str(), event->toString().c_str());
            iflMsg = event->getStr();
            delete event;
        } else {
            out->fatal(CALL_INFO, -1, "Error in %s: Received an event during complete() but it is not the expected type\n", getName().c_str());
        }
    }

}

/*
 * Lifecycle Phase #6: Finish
 * During the finish() phase, output the info that we received during complete()
 */
void SensorComponent::finish()
{
}

/*
 * Lifecycle Phase #7: Destruction
 * Clean up our output object
 * SST will delete links
 */
SensorComponent::~SensorComponent()
{
    delete out;
}

/*
 * Emergency Shutdown
 * Try sending the simulation a SIGTERM
 */
void SensorComponent::emergencyShutdown() {
    out->output("Uh-oh, my name is %s and I have to quit. I sent %u messages.\n", getName().c_str(), eventsSent);
}

/*
 * PrintStatus
 * Try sending the simulation a SIGUSR2
 */
void SensorComponent::printStatus(Output& sim_out) {
    sim_out.output("%s reporting. I have sent %u messages, received %u, and forwarded %u.\n",
            getName().c_str(), eventsSent, eventsReceived, eventsForwarded);
}
bool SensorComponent::mainTick( Cycle_t cycles)
{
  //Send an event across iflLink to generate compute
  iflLink->send(new SensorEvent("tick"));
  eventsSent++;
  //Check to see if all events have been sent, if so signal ready for complete
  if (eventsSent >= eventsToSend) {
      SensorEvent *ev = new SensorEvent("end");
      ev->setLast();
      iflLink->send(ev);
      primaryComponentOKToEndSim();
      return true;
  } else {
      return false;
  }
}
