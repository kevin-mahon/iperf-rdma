/*--------------------------------------------------------------- 
 * Copyright (c) 1999,2000,2001,2002,2003                              
 * The Board of Trustees of the University of Illinois            
 * All Rights Reserved.                                           
 *--------------------------------------------------------------- 
 * Permission is hereby granted, free of charge, to any person    
 * obtaining a copy of this software (Iperf) and associated       
 * documentation files (the "Software"), to deal in the Software  
 * without restriction, including without limitation the          
 * rights to use, copy, modify, merge, publish, distribute,        
 * sublicense, and/or sell copies of the Software, and to permit     
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions: 
 *
 *     
 * Redistributions of source code must retain the above 
 * copyright notice, this list of conditions and 
 * the following disclaimers. 
 *
 *     
 * Redistributions in binary form must reproduce the above 
 * copyright notice, this list of conditions and the following 
 * disclaimers in the documentation and/or other materials 
 * provided with the distribution. 
 * 
 *     
 * Neither the names of the University of Illinois, NCSA, 
 * nor the names of its contributors may be used to endorse 
 * or promote products derived from this Software without
 * specific prior written permission. 
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
 * ________________________________________________________________
 * National Laboratory for Applied Network Research 
 * National Center for Supercomputing Applications 
 * University of Illinois at Urbana-Champaign 
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________ 
 *
 * Server.cpp
 * by Mark Gates <mgates@nlanr.net>
 *     Ajay Tirumala (tirumala@ncsa.uiuc.edu>.
 * -------------------------------------------------------------------
 * A server thread is initiated for each connection accept() returns.
 * Handles sending and receiving data, and then closes socket.
 * Changes to this version : The server can be run as a daemon
 * ------------------------------------------------------------------- */

#define HEADERS()

#include "headers.h"
#include "Server.hpp"
#include "List.h"
#include "Extractor.h"
#include "Reporter.h"
#include "Locale.h"

/* -------------------------------------------------------------------
 * Stores connected socket and socket info.
 * ------------------------------------------------------------------- */

Server::Server( thread_Settings *inSettings ) {
    mSettings = inSettings;
    mBuf = NULL;

    // initialize buffer
    mBuf = new char[ mSettings->mBufLen ];
    FAIL_errno( mBuf == NULL, "No memory for buffer\n", mSettings );
}

/* -------------------------------------------------------------------
 * Destructor close socket.
 * ------------------------------------------------------------------- */

Server::~Server() {
    if ( mSettings->mSock != INVALID_SOCKET ) {
        int rc = close( mSettings->mSock );
        WARN_errno( rc == SOCKET_ERROR, "close" );
        mSettings->mSock = INVALID_SOCKET;
    }
    DELETE_ARRAY( mBuf );
}

void Server::Sig_Int( int inSigno ) {
}

/* ------------------------------------------------------------------- 
 * Receive data from the (connected) socket.
 * Sends termination flag several times at the end. 
 * Does not close the socket. 
 * ------------------------------------------------------------------- */ 
void Server::Run( void ) {
    long currLen; 
    max_size_t totLen = 0;
    struct UDP_datagram* mBuf_UDP  = (struct UDP_datagram*) mBuf; 

    ReportStruct *reportstruct = NULL;

    reportstruct = new ReportStruct;
    if ( reportstruct != NULL ) {
        reportstruct->packetID = 0;
        mSettings->reporthdr = InitReport( mSettings );
        do {
            // perform read 
            currLen = recv( mSettings->mSock, mBuf, mSettings->mBufLen, 0 ); 
        
            if ( isUDP( mSettings ) ) {
                // read the datagram ID and sentTime out of the buffer 
                reportstruct->packetID = ntohl( mBuf_UDP->id ); 
                reportstruct->sentTime.tv_sec = ntohl( mBuf_UDP->tv_sec  );
                reportstruct->sentTime.tv_usec = ntohl( mBuf_UDP->tv_usec ); 
		reportstruct->packetLen = currLen;
		gettimeofday( &(reportstruct->packetTime), NULL );
            } else {
		totLen += currLen;
	    }
        
            // terminate when datagram begins with negative index 
            // the datagram ID should be correct, just negated 
            if ( reportstruct->packetID < 0 ) {
                reportstruct->packetID = -reportstruct->packetID;
                currLen = -1; 
            }

	    if ( isUDP (mSettings)) {
		ReportPacket( mSettings->reporthdr, reportstruct );
            } else if ( !isUDP (mSettings) && mSettings->mInterval > 0) {
                reportstruct->packetLen = currLen;
                gettimeofday( &(reportstruct->packetTime), NULL );
                ReportPacket( mSettings->reporthdr, reportstruct );
            }

            if (mSettings->Output_file != NULL)
	        if ( fwrite( mBuf, currLen, 1, mSettings->Output_file ) < 0 )
	            fprintf( stderr, "Unable to write to the file stream\n");

        } while ( currLen > 0 ); 
        
        
        // stop timing 
        gettimeofday( &(reportstruct->packetTime), NULL );
        
	if ( !isUDP (mSettings)) {
		if(0.0 == mSettings->mInterval) {
                        reportstruct->packetLen = totLen;
                }
		ReportPacket( mSettings->reporthdr, reportstruct );
	}
        CloseReport( mSettings->reporthdr, reportstruct );
        
        // send a acknowledgement back only if we're NOT receiving multicast 
        if ( isUDP( mSettings ) && !isMulticast( mSettings ) ) {
            // send back an acknowledgement of the terminating datagram 
            write_UDP_AckFIN( ); 
        }
    } else {
        FAIL(1, "Out of memory! Closing server thread\n", mSettings);
    }

    Mutex_Lock( &clients_mutex );     
    Iperf_delete( &(mSettings->peer), &clients ); 
    Mutex_Unlock( &clients_mutex );

    DELETE_PTR( reportstruct );
    EndReport( mSettings->reporthdr );
} 
// end Recv 

void Server::RunRDMA( void ) {

    struct ibv_send_wr *bad_wr;
    rdma_Ctrl_Blk *cb = mSettings->mCtrlBlk;
    int rc, i;
    struct remote_u *rmt_u;
    struct iperf_rdma_control_msg *cm;
    struct iperf_rdma_io_u *io_u;
    struct ibv_wc wc;
    struct ibv_wc *wcp; // completion work request list
    int nr;
    struct ibv_recv_wr *bad_recv_wr;
	
    long currLen; 
    max_size_t totLen = 0;
    struct UDP_datagram* mBuf_UDP  = (struct UDP_datagram*) mBuf; 

    ReportStruct *reportstruct = NULL;

    reportstruct = new ReportStruct;

    wcp = (struct ibv_wc *) malloc ( cb->rdma_iodepth * \
				     sizeof (struct ibv_wc) );
    FAIL_errno( wcp == NULL, "malloc", mSettings );
    memset(wcp, '\0', cb->rdma_iodepth * sizeof (struct ibv_wc));

    iperf_rdma_poll_wait_control_msg(cb, IBV_WC_RECV, &wc);

    switch (ntohl(cb->recv_buf.mode)) {
    case kRDMAOpc_RDMA_Write:
	    cb->rdma_opcode = kRDMAOpc_RDMA_Write;
	    break;
    case kRDMAOpc_RDMA_Read:
	    cb->rdma_opcode = kRDMAOpc_RDMA_Read;
	    break;
    case kRDMAOpc_Send_Recv:
	    cb->rdma_opcode = kRDMAOpc_Send_Recv;
	    break;
    default:
	    break;
    }
    cb->rdma_iodepth = ntohl(cb->recv_buf.iodepth);
    cb->buflen = ntohl(cb->recv_buf.size);

    // enlarge recv queue depth
    if (cb->rdma_opcode == kRDMAOpc_Send_Recv) {
	if (cb->rdma_iodepth * 2 > IPERF_RDMA_MAX_IO_DEPTH)
	    cb->rdma_iodepth = IPERF_RDMA_MAX_IO_DEPTH;
	else
            cb->rdma_iodepth *= 2;

        cb->rdma_iodepth = IPERF_RDMA_MAX_IO_DEPTH;
    }

    iperf_setup_local_buf( cb );

    switch (cb->rdma_opcode) {
    case kRDMAOpc_RDMA_Write:
    case kRDMAOpc_RDMA_Read:
        cb->send_buf.mode = htonl(cb->rdma_opcode);
        cb->send_buf.iodepth = htonl(cb->rdma_iodepth);
        cb->send_buf.size = htonl(cb->buflen);

        cm = &cb->send_buf;
        for (i = 0; i < cb->rdma_iodepth; i++) {
            rmt_u = &cm->rmt_us[i];
            io_u = &cb->io_us[i];
            rmt_u->buf = htonll((uint64_t) (unsigned long)io_u->addr);
            rmt_u->rkey = htonl(io_u->mr->rkey);
            rmt_u->size = htonl(io_u->size);
	}
        break;
    case kRDMAOpc_Send_Recv:
        cb->send_buf.mode = htonl(cb->rdma_opcode);
        cb->send_buf.iodepth = htonl(cb->rdma_iodepth);
        cb->send_buf.size = htonl(cb->buflen);
	break;
    default:
        break;
    }

    if (cb->rdma_opcode == kRDMAOpc_Send_Recv) {
        // setup and post all the receive buffers into receive queue
        iperf_rdma_setup_recvbuf(cb);
	rc = iperf_rdma_post_all_recvbuf(cb);
        FAIL( rc != 0, "post recv tasks", mSettings );
	printf("post recv buffers success\n");
    }

    // send back credit
    rc = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
    FAIL_errno( rc != 0, "ibv_post_send", mSettings );

    // get send completion event
    iperf_rdma_poll_wait_control_msg(cb, IBV_WC_SEND, &wc);

    if ( reportstruct != NULL ) {
        reportstruct->packetID = 0;
        mSettings->reporthdr = InitReport( mSettings );
        
        do {
            switch (cb->rdma_opcode) {
            case kRDMAOpc_RDMA_Write:
            case kRDMAOpc_RDMA_Read:
                // wait for completion event - graceful teardown
                // deal with send/recv
		WARN( 1, "Nice, START one-sided testing~~~" );
		sleep(100);
                break;
            case kRDMAOpc_Send_Recv:
                do {
			nr = iperf_rdma_poll_comp(cb, cb->rdma_iodepth, wcp);
		} while (nr == 0);
                break;
            default:
                break;
            }

	    currLen = 0;
	    if (cb->rdma_opcode == kRDMAOpc_Send_Recv) {
                // repost completed receive buffer
                for (i = 0; i < nr; i++) {
                    io_u = &cb->io_us[wcp[i].wr_id];
		    rc = ibv_post_recv(cb->qp, &io_u->rq_wr, &bad_recv_wr);
		    WARN_errno( rc != 0, "ibv_post_recv" );
		    currLen += io_u->size;
		}
	    }

            if ( isUDP( mSettings ) ) {
                // read the datagram ID and sentTime out of the buffer 
                reportstruct->packetID = ntohl( mBuf_UDP->id ); 
                reportstruct->sentTime.tv_sec = ntohl( mBuf_UDP->tv_sec  );
                reportstruct->sentTime.tv_usec = ntohl( mBuf_UDP->tv_usec ); 
		reportstruct->packetLen = currLen;
		gettimeofday( &(reportstruct->packetTime), NULL );
            } else {
		totLen += currLen;
	    }
        
            // terminate when datagram begins with negative index 
            // the datagram ID should be correct, just negated 
	    if ( reportstruct->packetID < 0 ) {
                reportstruct->packetID = -reportstruct->packetID;
                currLen = -1; 
            }

	    if ( isUDP (mSettings)) {
		ReportPacket( mSettings->reporthdr, reportstruct );
            } else if ( !isUDP (mSettings) && mSettings->mInterval > 0) {
                reportstruct->packetLen = currLen;
                gettimeofday( &(reportstruct->packetTime), NULL );
                ReportPacket( mSettings->reporthdr, reportstruct );
            }
            
        } while ( currLen > 0 ); 
        
        
        // stop timing 
        gettimeofday( &(reportstruct->packetTime), NULL );
        
	if ( !isUDP (mSettings)) {
		if(0.0 == mSettings->mInterval) {
                        reportstruct->packetLen = totLen;
                }
		ReportPacket( mSettings->reporthdr, reportstruct );
	}
        CloseReport( mSettings->reporthdr, reportstruct );
        
        // send a acknowledgement back only if we're NOT receiving multicast 
        if ( isUDP( mSettings ) && !isMulticast( mSettings ) ) {
            // send back an acknowledgement of the terminating datagram 
            write_UDP_AckFIN( ); 
        }
    } else {
        FAIL(1, "Out of memory! Closing server thread\n", mSettings);
    }

    Mutex_Lock( &clients_mutex );     
    Iperf_delete( &(mSettings->peer), &clients ); 
    Mutex_Unlock( &clients_mutex );

    DELETE_PTR( reportstruct );
    EndReport( mSettings->reporthdr );
    
    return;
} 

/* ------------------------------------------------------------------- 
 * Send an AckFIN (a datagram acknowledging a FIN) on the socket, 
 * then select on the socket for some time. If additional datagrams 
 * come in, probably our AckFIN was lost and they are re-transmitted 
 * termination datagrams, so re-transmit our AckFIN. 
 * ------------------------------------------------------------------- */ 

void Server::write_UDP_AckFIN( ) {

    int rc; 

    fd_set readSet; 
    FD_ZERO( &readSet ); 

    struct timeval timeout; 

    int count = 0; 
    while ( count < 10 ) {
        count++; 

        UDP_datagram *UDP_Hdr;
        server_hdr *hdr;

        UDP_Hdr = (UDP_datagram*) mBuf;

        if ( mSettings->mBufLen > (int) ( sizeof( UDP_datagram )
                                          + sizeof( server_hdr ) ) ) {
            Transfer_Info *stats = GetReport( mSettings->reporthdr );
            hdr = (server_hdr*) (UDP_Hdr+1);

            hdr->flags        = htonl( HEADER_VERSION1 );
            hdr->total_len1   = htonl( (long) (stats->TotalLen >> 32) );
            hdr->total_len2   = htonl( (long) (stats->TotalLen & 0xFFFFFFFF) );
            hdr->stop_sec     = htonl( (long) stats->endTime );
            hdr->stop_usec    = htonl( (long)((stats->endTime - (long)stats->endTime)
                                              * rMillion));
            hdr->error_cnt    = htonl( stats->cntError );
            hdr->outorder_cnt = htonl( stats->cntOutofOrder );
            hdr->datagrams    = htonl( stats->cntDatagrams );
            hdr->jitter1      = htonl( (long) stats->jitter );
            hdr->jitter2      = htonl( (long) ((stats->jitter - (long)stats->jitter) 
                                               * rMillion) );

        }

        // write data 
        write( mSettings->mSock, mBuf, mSettings->mBufLen ); 

        // wait until the socket is readable, or our timeout expires 
        FD_SET( mSettings->mSock, &readSet ); 
        timeout.tv_sec  = 1; 
        timeout.tv_usec = 0; 

        rc = select( mSettings->mSock+1, &readSet, NULL, NULL, &timeout ); 
        FAIL_errno( rc == SOCKET_ERROR, "select", mSettings ); 

        if ( rc == 0 ) {
            // select timed out 
            return; 
        } else {
            // socket ready to read 
            rc = read( mSettings->mSock, mBuf, mSettings->mBufLen ); 
            WARN_errno( rc < 0, "read" );
            if ( rc <= 0 ) {
                // Connection closed or errored
                // Stop using it.
                return;
            }
        } 
    } 

    fprintf( stderr, warn_ack_failed, mSettings->mSock, count ); 
} 
// end write_UDP_AckFIN 

