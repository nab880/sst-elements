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

#ifndef COMPONENTS_FIREFLY_FUNCSM_GATHERV_H
#define COMPONENTS_FIREFLY_FUNCSM_GATHERV_H

#include "funcSM/api.h"
#include "funcSM/event.h"
#include "info.h"
#include "ctrlMsg.h"

namespace SST {
namespace Firefly {

class QQQ {

  public:
    QQQ( int degree, int myRank, int size, int root  ) :
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

    void foo( int x, std::vector<int>& map ) {
        if ( m_root ) {
            if ( x == m_root ) {
                map.push_back(0);
            } else if ( 0 == x )  {
                map.push_back( m_root);
            } else { 
                map.push_back(x);
            }
        } else { 
            map.push_back(x);
        }

        for ( int i = 0; i < m_degree; i++ ) {
            int child = (x * m_degree) + i + 1; 
            if ( child < m_size ) {
                foo( child, map );
            }
        }
    }

    std::vector<int> getMap() {
        std::vector<int> map;
        foo( 0, map ); 
        return map;
    }

    int myRank() { return m_myRank; }
    int size() { return m_size; }

    int parent() { return m_parent; }

    size_t numChildren() { return m_numChildren; }

    int calcChild( int i ) {
        int child = (m_myVirtRank * m_degree) + i + 1;
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


class GathervFuncSM :  public FunctionSMInterface
{
    enum { WaitUp, SendUp } m_state;

    enum { WaitUpRecv, WaitUpSend, WaitUpRecvBody } m_waitUpState;
    enum { SendUpSend, SendUpWait, SendUpSendBody } m_sendUpState;

    static const int GathervTag = 0xf0020000;


  public:
    GathervFuncSM( int verboseLevel, Output::output_location_t loc,
            Info* info, SST::Link* progressLink,
            ProtocolAPI*, SST::Link* );

    virtual void handleEnterEvent( SST::Event *e);
    virtual void handleSelfEvent( SST::Event *e);
    virtual void handleProgressEvent( SST::Event *e );

    virtual const char* name() {
       return "Gatherv"; 
    }

  private:

    bool waitUp();
    bool sendUp();
    void doRoot();
    uint32_t    genTag( int i = 0 ) {
        return GathervTag | i << 8 | (m_seq & 0xff);
    } 

    SST::Link*          m_toProgressLink;
    SST::Link*          m_selfLink;
    CtrlMsg*            m_ctrlMsg;
    GatherEnterEvent*  m_event;
    QQQ*                m_qqq;
    bool                m_pending;
    CtrlMsg::CommReq    m_sendReq; 
    CtrlMsg::CommReq    m_recvReq; 
    std::vector<CtrlMsg::CommReq>  m_recvReqV;

    bool                m_waitUpPending;
    std::vector<int>    m_waitUpSize;
    std::vector<unsigned char>  m_recvBuf;
    int                 m_intBuf;

    unsigned int        m_count; 
    bool                m_sendUpPending;
    int                 m_seq;
    int                 m_waitUpDelay;
    bool                m_waitUpTest;

    int                 m_sendUpDelay;
    bool                m_sendUpTest;
};
        
}
}

#endif
