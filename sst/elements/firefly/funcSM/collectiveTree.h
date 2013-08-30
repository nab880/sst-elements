// Copyright 2013 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2013, Sandia Corporation
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef COMPONENTS_FIREFLY_FUNCSM_COLLECTIVE_H
#define COMPONENTS_FIREFLY_FUNCSM_COLLECTIVE_H

#include "funcSM/api.h"
#include "funcSM/event.h"
#include "info.h"
#include "ctrlMsg.h"

namespace SST {
namespace Firefly {

class YYY {

  public:
    YYY( int degree, int myRank, int size, int root  ) :
        m_degree( degree ),
        m_myRank( myRank ),
        m_size( size ),
        m_numChildren(0),
        m_myVirtRank( myRank ),
        m_root( root ),
        m_parent( -1 )
    {
        if ( root > 0 ) {
            if ( root == m_myRank ) {
                m_myVirtRank = 0;
            } else if ( 0 == m_myRank )  {
                m_myVirtRank = root;
            }
        }

        for ( int i = 0; i < m_degree; i++ ) {
            if ( calcChild( i ) < m_size ) {
                ++m_numChildren;
            }
        }

        if ( m_myVirtRank > 0 ) {
            int tmp = m_myVirtRank % m_degree;
            tmp = 0 == tmp ? m_degree : tmp ;
            m_parent = (m_myVirtRank - tmp ) / m_degree;

            if ( m_parent == 0 ) {
                m_parent = m_root;
            } else if ( m_parent == m_root ) {
                m_parent = 0;
            }
        }
    }

    int myRank() { return m_myRank; }

    int size() { return m_size; }

    int parent() { return m_parent; }

    size_t numChildren() { return m_numChildren; }

    int calcChild( int i ) {
        int child = (m_myVirtRank * m_degree) + i + 1;
        // ummm, child can never be 0
        if ( child == 0 ) {
            child = m_root; 
        }  else if ( child == m_root ) {
            child = 0;
        }
        return child; 
    }

  private:

    int m_degree;
    int m_myRank;
    int m_size;
    int m_numChildren;
    int m_myVirtRank;
    int m_root;
    int m_parent;
};

class CollectiveTreeFuncSM :  public FunctionSMInterface
{
    enum { WaitUp, SendUp, WaitDown, SendDown } m_state;

    static const int CollectiveTag = 0xf0000000;

  public:
    CollectiveTreeFuncSM( int verboseLevel, Output::output_location_t loc,
            Info* info, SST::Link* progressLink, ProtocolAPI*, SST::Link* );

    virtual void handleEnterEvent( SST::Event *e);
    virtual void handleSelfEvent( SST::Event *e);
    virtual void handleProgressEvent( SST::Event *e );

    virtual const char* name() {
       return "CollectiveTree"; 
    }

  private:

    uint32_t    genTag() {
        return CollectiveTag | (m_seq & 0xffff);
    }

    bool                    m_test;
    int                     m_delay;
    bool                    m_pending;
    CollectiveEnterEvent*   m_event;
    SST::Link*              m_toProgressLink;
    SST::Link*              m_selfLink;
    CtrlMsg*                m_ctrlMsg;
    std::vector<CtrlMsg::CommReq>  m_recvReqV;
    std::vector<void*>  m_bufV;
    CtrlMsg::CommReq    m_sendReq; 
    unsigned int        m_count;
    size_t              m_bufLen;
    YYY*                m_yyy;
    int                 m_seq;
};
        
}
}

#endif
