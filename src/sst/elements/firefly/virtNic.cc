// Copyright 2013-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2013-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// Portions copyright (c) 2026, Hewlett Packard Enterprise Development LP
// SPDX-FileCopyrightText: Copyright Hewlett Packard Enterprise Development LP
// SPDX-License-Identifier: BSD-3-Clause
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "sst_config.h"

#include <sst/core/link.h>
#include <sst/core/params.h>
#include "virtNic.h"
#include "nic.h"

#include <cstdlib>

using namespace SST::Firefly;
using namespace SST;

struct VirtNic::FeatureState {
    std::optional<FireflyCollectiveEndpoint> collective_endpoint;
};

VirtNic::VirtNic( ComponentId_t id, Params& params ) :
	SubComponent(id),
    m_realNicId(-1),
    m_notifyGetDone(NULL),
    m_notifySendPioDone(NULL),
    m_notifyRecvDmaDone(NULL),
    m_notifyNeedRecv(NULL),
	    m_curNicQdepth(0),
	    m_blockedCallback(NULL),
	    m_nextTimeSlot(0)
{
    m_dbg.init("@t:VirtNic::@p():@l ",
        params.find<uint32_t>("verboseLevel",0),
        0,
        Output::STDOUT );

    m_maxNicQdepth = params.find<int>("maxNicQdepth",32);
    m_latPerSend_ns = params.find<int>("latPerSend_ns",2);

    m_toNicLink = configureLink( params.find<std::string>("portName","nic"),
			"1 ns", new Event::Handler<VirtNic,&VirtNic::handleEvent>(this) );

    assert( m_toNicLink );
}

VirtNic::~VirtNic()
{
    if ( m_notifyGetDone ) delete m_notifyGetDone;
    if ( m_notifySendPioDone ) delete m_notifySendPioDone;
    if ( m_notifyRecvDmaDone ) delete m_notifyRecvDmaDone;
    if ( m_notifyNeedRecv ) delete m_notifyNeedRecv;
}

VirtNic::FeatureState& VirtNic::ensureFeatureState()
{
    if ( !m_featureState ) m_featureState = std::make_unique<FeatureState>();
    return *m_featureState;
}

void VirtNic::init( unsigned int phase )
{
    m_dbg.debug(CALL_INFO,1,0,"phase=%d\n",phase);

    if ( phase > 0 ) {
        Event* raw = nullptr;
        while ( (raw = m_toNicLink->recvUntimedData()) != nullptr ) {
            if ( auto* ev = dynamic_cast<NicInitEvent*>(raw) ) {
                if ( m_realNicId != -1 ) {
                    m_dbg.fatal(CALL_INFO, -1, "Duplicate Firefly NIC initialization event\n");
                }
                m_realNicId = ev->node;
                m_coreId = ev->vNic;
                m_numCores = ev->num_vNics;

                char buffer[100];
                snprintf(buffer,100,"@t:%d:%d:VirtNic::@p():@l ", m_realNicId, m_coreId );
                m_dbg.setPrefix( buffer );
                m_dbg.debug(CALL_INFO,1,0,"we are nic=%d core=%d\n", m_realNicId, m_coreId );
                delete ev;
                continue;
            }
            if ( auto* ev = dynamic_cast<NicCollectiveInitEvent*>(raw) ) {
                if ( m_realNicId == -1 ||
                        (m_featureState && m_featureState->collective_endpoint) ) {
                    m_dbg.fatal(CALL_INFO, -1, "Invalid Firefly collective route publication\n");
                }
                auto& endpoint = ensureFeatureState().collective_endpoint.emplace(*this);
                if ( !endpoint.publish(ev->participant) ) {
                    m_dbg.fatal(CALL_INFO, -1, "Invalid Firefly collective route publication\n");
                }
                delete ev;
                continue;
            }
            delete raw;
            m_dbg.fatal(CALL_INFO, -1, "Unknown Firefly NIC initialization event\n");
        }
        if ( phase == 1 && m_realNicId == -1 ) {
            m_dbg.fatal(CALL_INFO, -1, "Missing Firefly NIC initialization event\n");
        }
    }
}

void VirtNic::handleEvent( Event* ev )
{
    NicRespBaseEvent* event = static_cast<NicRespBaseEvent*>(ev);

    m_dbg.debug(CALL_INFO,2,0,"type=%d\n",event->base_type);

    switch( event->base_type ) {
    case NicRespBaseEvent::Msg:
        handleMsgEvent( static_cast<NicRespEvent*>(ev ) );
        break;
    case NicRespBaseEvent::Shmem:
        handleShmemEvent( static_cast<NicShmemRespEvent*>(ev ) );
        break;
    case NicRespBaseEvent::NetworkIO:
        handleNetworkIOEvent( static_cast<NicNetworkIORespBaseEvent*>(ev ) );
        break;
    case NicRespBaseEvent::Collective:
        handleCollectiveEvent( static_cast<NicCollectiveRespBaseEvent*>(ev ) );
        break;
    default:
        m_dbg.fatal(CALL_INFO, -1, "Unknown NIC response event type %d\n", event->base_type);
    }
    delete ev;
}

void VirtNic::notifyReadyIfPossible()
{
    auto* state = featureState();
    if ( state && state->collective_endpoint && m_curNicQdepth < m_maxNicQdepth ) {
        state->collective_endpoint->notifyReadyIfPossible();
    }
}

void VirtNic::releaseNicCommandSlot()
{
    if ( m_curNicQdepth <= 0 ) {
        m_dbg.fatal(CALL_INFO, -1, "NIC response has no outstanding host command\n");
    }
    --m_curNicQdepth;
    if ( m_blockedCallback ) {
        Callback callback = std::move(m_blockedCallback);
        m_blockedCallback = nullptr;
        callback();
    }
}

void VirtNic::handleCollectiveEvent( NicCollectiveRespBaseEvent* event )
{
    auto* state = featureState();
    if ( collectiveParticipant(0) == nullptr || state == nullptr || !state->collective_endpoint ) {
        m_dbg.fatal(CALL_INFO, -1, "Collective response received without a published route\n");
    }

    switch ( event->type ) {
    case NicCollectiveRespBaseEvent::Result: {
        auto* result = static_cast<NicCollectiveResultEvent*>(event);
        state->collective_endpoint->receiveResult(result->result);
        notifyReadyIfPossible();
        break;
    }
    case NicCollectiveRespBaseEvent::SubmitAccepted: {
        const uint64_t invocation_id =
            static_cast<NicCollectiveSubmitAcceptedEvent*>(event)->invocation_id;
        state->collective_endpoint->submitAccepted(invocation_id);
        releaseNicCommandSlot();
        notifyReadyIfPossible();
        break;
    }
    default:
        m_dbg.fatal(CALL_INFO, -1, "Unknown collective response type %d\n", event->type);
    }
}

void VirtNic::handleMsgEvent( NicRespEvent* event )
{
    m_dbg.debug(CALL_INFO,2,0,"type=%d\n",event->type);
    switch( event->type ) {
    case NicRespEvent::Get:
        (*m_notifyGetDone)( event->key );
        break;
    case NicRespEvent::PioSend:
        (*m_notifySendPioDone)( event->key );
        break;
    case NicRespEvent::DmaRecv:
        (*m_notifyRecvDmaDone)( calcNodeId( event->node, event->src_vNic ),
                    event->tag, event->len, event->key  );
        break;
    case NicRespEvent::NeedRecv:
        (*m_notifyNeedRecv)( calcNodeId( event->node, event->src_vNic), event->len );
        break;
    default:
        assert(0);
    }
}
void VirtNic::handleShmemEvent( NicShmemRespBaseEvent* event )
{
    NicShmemRespBaseEvent* ev = static_cast<NicShmemRespBaseEvent*>(event);

   	m_dbg.debug(CALL_INFO,2,0,"calling callback\n");
   	ev->callback();

	m_dbg.debug(CALL_INFO,2,0," %d %d\n", m_curNicQdepth, m_maxNicQdepth);
    releaseNicCommandSlot();
    notifyReadyIfPossible();
}

void VirtNic::handleNetworkIOEvent( NicNetworkIORespBaseEvent* event )
{
    NicNetworkIORespBaseEvent* ev = static_cast<NicNetworkIORespBaseEvent*>(event);

   	m_dbg.debug(CALL_INFO,2,0,"[VirtNic] calling NetworkIO callback\n");
   	ev->callback();

	m_dbg.debug(CALL_INFO,2,0," %d %d\n", m_curNicQdepth, m_maxNicQdepth);
    releaseNicCommandSlot();
    notifyReadyIfPossible();
}

SST::Collective::CollectiveEndpoint* VirtNic::collectiveEndpoint() const
{
    auto* state = featureState();
    return collectiveParticipant(0) == nullptr || state == nullptr ? nullptr :
        &*state->collective_endpoint;
}

const SST::Collective::AcceptedParticipantHandle* VirtNic::collectiveParticipant(
        uint32_t local_slot ) const
{
    auto* state = featureState();
    return state == nullptr || !state->collective_endpoint ? nullptr :
        state->collective_endpoint->participant(local_slot);
}

bool VirtNic::collectiveCommandSlotAvailable() const
{
    return m_curNicQdepth < m_maxNicQdepth;
}

void VirtNic::sendCollectiveCommand( NicCollectiveSubmitCmdEvent* event )
{
    sendCmd(0, event);
}

[[noreturn]] void VirtNic::collectiveFatal( const char* reason )
{
    m_dbg.fatal(CALL_INFO, -1, "%s\n", reason);
    std::abort();
}

bool VirtNic::canDmaSend()
{
    m_dbg.debug(CALL_INFO,1,0,"\n");
    //return m_nic.canDmaSend( this );
    return true;
}

bool VirtNic::canDmaRecv()
{
    m_dbg.debug(CALL_INFO,1,0,"\n");
//    return m_nic.canDmaRecv( this );
    return true;
}

void VirtNic::dmaRecv( int src, int tag, std::vector<IoVec>& vec, void* key )
{
    m_dbg.debug(CALL_INFO,2,0,"src=%d\n",src);
    m_toNicLink->send(calcDelay(), new NicCmdEvent( NicCmdEvent::DmaRecv,
            calcCoreId(src), calcRealNicId(src), tag, vec, key ) );
}

void VirtNic::pioSend( int vn, int dest, int tag, std::vector<IoVec>& vec, void* key )
{
    m_dbg.debug(CALL_INFO,2,0,"dest=%d\n",dest);
    m_toNicLink->send(calcDelay(), new NicCmdEvent( NicCmdEvent::PioSend,
			calcCoreId(dest), calcRealNicId(dest), tag, vec, key, vn ) );
}

void VirtNic::get( int node, int tag, std::vector<IoVec>& vec, void* key )
{
    m_dbg.debug(CALL_INFO,2,0,"node=%d\n",node);
    m_toNicLink->send(calcDelay(), new NicCmdEvent( NicCmdEvent::Get,
			calcCoreId(node), calcRealNicId(node), tag, vec, key ) );
}

void VirtNic::regMem( int node, int tag, std::vector<IoVec>& vec, void* key )
{
    m_dbg.debug(CALL_INFO,2,0,"node=%d\n",node);
    m_toNicLink->send(calcDelay(), new NicCmdEvent( NicCmdEvent::RegMemRgn,
			calcCoreId(node), calcRealNicId(node), tag, vec, key ) );
}

void VirtNic::shmemInit( Hermes::Vaddr addr, Callback callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemInitCmdEvent( addr, callback ) );
}

void VirtNic::shmemFence( Callback callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemFenceCmdEvent( callback ) );
}

void VirtNic::shmemRegMem( Hermes::MemAddr& addr, Hermes::Vaddr realAddr, size_t len, Callback callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemRegMemCmdEvent( addr, realAddr, len, callback ) );
}

void VirtNic::shmemGet( int node, Hermes::Vaddr dest, Hermes::Vaddr src, size_t len, Callback callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemGetCmdEvent( calcCoreId(node), calcRealNicId(node), dest, src, len, callback ) );
}

void VirtNic::shmemGetv( int node, Hermes::Vaddr src, Hermes::Value::Type type, CallbackV callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemGetvCmdEvent( calcCoreId(node), calcRealNicId(node), src, type, callback ) );
}

void VirtNic::shmemWait( Hermes::Vaddr addr, Hermes::Shmem::WaitOp op, Hermes::Value& value, Callback callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemOpCmdEvent( addr, op, value, callback ) );
}

void VirtNic::shmemPut( int node, Hermes::Vaddr dest, Hermes::Vaddr src, size_t len, Callback callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemPutCmdEvent( calcCoreId(node), calcRealNicId(node), dest, src, len, callback ) );
}

void VirtNic::shmemPutOp( int node, Hermes::Vaddr dest, Hermes::Vaddr src, size_t len,
            Hermes::Shmem::ReduOp op, Hermes::Value::Type dataType, Callback callback )
{
    m_dbg.debug(CALL_INFO,2,0," %d\n",op);
    sendCmd(0, new NicShmemPutCmdEvent( calcCoreId(node), calcRealNicId(node), dest, src, len, op, dataType, callback ) );
}

void VirtNic::shmemPutv( int node, Hermes::Vaddr dest, Hermes::Value& value )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemPutvCmdEvent( calcCoreId(node), calcRealNicId(node), dest, value ) );
}

void VirtNic::shmemSwap( int node, Hermes::Vaddr dest, Hermes::Value& value , CallbackV callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemSwapCmdEvent( calcCoreId(node), calcRealNicId(node), dest, value, callback ) );
}

void VirtNic::shmemCswap( int node, Hermes::Vaddr dest, Hermes::Value& cond, Hermes::Value& value , CallbackV callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemCswapCmdEvent( calcCoreId(node), calcRealNicId(node), dest, cond, value, callback ) );
}

void VirtNic::shmemAdd( int node, Hermes::Vaddr dest, Hermes::Value& value )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemAddCmdEvent( calcCoreId(node), calcRealNicId(node), dest, value ) );
}

void VirtNic::shmemFadd( int node, Hermes::Vaddr dest, Hermes::Value& value, CallbackV callback )
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    sendCmd(0, new NicShmemFaddCmdEvent( calcCoreId(node), calcRealNicId(node), dest, value, callback ) );
}

void VirtNic::setNotifyOnRecvDmaDone(
                VirtNic::HandlerBase4Args<int,int,size_t,void*>* functor)
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    m_notifyRecvDmaDone = functor;
}

void VirtNic::setNotifyOnSendPioDone(VirtNic::HandlerBase<void*>* functor)
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    m_notifySendPioDone = functor;
}

void VirtNic::setNotifyOnGetDone(VirtNic::HandlerBase<void*>* functor)
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    m_notifyGetDone = functor;
}

void VirtNic::setNotifyNeedRecv(
                VirtNic::HandlerBase2Args<int,size_t>* functor)
{
    m_dbg.debug(CALL_INFO,2,0,"\n");
    m_notifyNeedRecv = functor;
}

void VirtNic::networkIORead( int targetNid, Hermes::Vaddr dest, size_t len, std::function<void(int)> callback )
{
    m_dbg.debug(CALL_INFO,2,0,"dest=%#" PRIx64 " len=%zu\n", dest, len);
    sendCmd(0, new NicNetworkIOReadCmdEvent(  targetNid, dest, len, callback ) );
}

void VirtNic::networkIOWrite( int targetNid, Hermes::Vaddr src, size_t len, std::function<void(int)> callback )
{
    m_dbg.debug(CALL_INFO,2,0,"src=%#" PRIx64 " len=%zu\n", src, len);
    sendCmd(0, new NicNetworkIOWriteCmdEvent(  targetNid, src, len, callback ) );
}
