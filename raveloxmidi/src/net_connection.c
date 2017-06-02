/*
 This file is part of raveloxmidi.
 
 Copyright (C) 2014 Dave Kelly
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software Foundation,
 Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA 
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include <arpa/inet.h>

#include <errno.h>
extern int errno;

#include "midi_journal.h"
#include "net_connection.h"
#include "rtp_packet.h"
#include "utils.h"

#include "raveloxmidi_config.h"
#include "logging.h"

static net_ctx_t **ctx;
uint8_t _max_ctx = 0;

void net_ctx_reset( net_ctx_t *ctx )
{
    journal_t *journal;
    
    if( ! ctx ) return;
    
    ctx->used = 0;
    ctx->ssrc = 0;
    ctx->send_ssrc = 0;
    ctx->initiator = 0;
    ctx->seq = 0x0000;
    FREENULL( (void **)&(ctx->ip_address) );
    journal_destroy( &(ctx->journal) );
    
    journal_init( &journal );
    ctx->journal = journal;
    
    ctx->data_port = 0;
    ctx->control_port = 0;
}

void debug_net_ctx_dump( net_ctx_t *ctx )
{
    if( ! ctx ) return;
    
    logging_printf( LOGGING_DEBUG, "CTX(\n");
    logging_printf( LOGGING_DEBUG, "\tUsed=%d\n", ctx->used);
    logging_printf( LOGGING_DEBUG, "\tssrc=%08x\n", ctx->ssrc);
    logging_printf( LOGGING_DEBUG, "\tsend_ssrc=%08x\n", ctx->send_ssrc);
    logging_printf( LOGGING_DEBUG, "\tinitiator=%08x\n", ctx->initiator);
    logging_printf( LOGGING_DEBUG, "\tseq=%08x (%08d)\n", ctx->seq, ctx->seq);
    logging_printf( LOGGING_DEBUG, "\thost=%s (control=%u data=%u))\n", ctx->ip_address, ctx->control_port, ctx->data_port);
}

static void net_ctx_set( net_ctx_t *ctx, uint32_t ssrc, uint32_t initiator, uint32_t send_ssrc, uint32_t seq, uint16_t port, char *ip_address )
{
    if( ! ctx ) return;
    
    if( ctx->used > 0 ) return;
    
    net_ctx_reset( ctx );
    
    ctx->used = 1;
    ctx->ssrc = ssrc;
    ctx->send_ssrc = send_ssrc;
    ctx->initiator = initiator;
    ctx->seq = seq;
    ctx->control_port = port;
    ctx->start = time( NULL );
    
    ctx->ip_address = ( char *) strdup( ip_address );
    
}

static net_ctx_t * new_net_ctx( void )
{
    net_ctx_t *new_ctx;
    journal_t *journal;
    
    new_ctx = ( net_ctx_t * ) malloc( sizeof( net_ctx_t ) );
    
    memset( new_ctx, 0, sizeof( net_ctx_t ) );
    new_ctx->seq = 0x0000;
    
    journal_init( &journal );
    
    new_ctx->journal = journal;
    
    return new_ctx;
    
}

void net_ctx_init( void )
{
    uint8_t i;
    
    _max_ctx = atoi( config_get("network.max_connections") );
    
    if( _max_ctx == 0 ) _max_ctx = 1;
    
    ctx = (net_ctx_t **)malloc( sizeof( net_ctx_t * ) * _max_ctx );
    
    for( i = 0 ; i < _max_ctx ; i++ )
    {
        ctx[ i ] = new_net_ctx();
    }
}


void net_ctx_destroy( void )
{
    uint8_t i;
    
    for( i = 0 ; i < _max_ctx ; i++ )
    {
        if( ctx[ i ] )
        {
            net_ctx_reset( ctx[i] );
            journal_destroy( & (ctx[i]->journal) );
            free( ctx[i] );
            ctx[i] = NULL;
        }
    }
    free( ctx );
    ctx = NULL;
}

int net_ctx_is_used( uint8_t id )
{
    if( id < _max_ctx ) return( ctx[id]->used > 0 );
    return 0;
}

net_ctx_t * net_ctx_find_by_id( uint8_t id )
{
    if( id < _max_ctx )
    {
        if( ctx[id] )
        {
            if( ctx[id]->used > 0 )
            {
                return ctx[id];
            }
        }
    }
    return NULL;
}

net_ctx_t * net_ctx_find_by_ssrc( uint32_t ssrc)
{
    uint8_t i;
    
    for( i = 0 ; i < _max_ctx ; i++ )
    {
        if( ctx[i] )
        {
            if( ctx[i]->used > 0 )
            {
                if( ctx[i]->ssrc == ssrc )
                {
                    return ctx[i];
                }
            }
        }
    }
    
    return NULL;
}

net_ctx_t * net_ctx_register( uint32_t ssrc, uint32_t initiator, char *ip_address, uint16_t port )
{
    uint8_t i;
    
    for( i = 0 ; i < _max_ctx ; i++ )
    {
        if( ctx[i] )
        {
            if( ctx[i]->used == 0 )
            {
                time_t now = time( NULL );
                unsigned int send_ssrc = rand_r( (unsigned int *)&now );
                
                net_ctx_set( ctx[i], ssrc, initiator, send_ssrc, 0x638F, port, ip_address );
                return ctx[i];
            }
        }
    }
    
    logging_printf( LOGGING_WARN, "No free connection slots available\n");
    return NULL;
}

void net_ctx_add_journal_note( uint8_t ctx_id , midi_note_packet_t *note_packet )
{
    net_ctx_t *ctx = NULL;
    
    if( ctx_id > _max_ctx - 1 ) return;
    
    if( ! note_packet ) return;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx) return;
    
    midi_journal_add_note( ctx->journal, ctx->seq, note_packet );
}

void debug_ctx_journal_dump( uint8_t ctx_id )
{
    net_ctx_t *ctx = NULL;
    
    if( ctx_id > _max_ctx - 1 ) return;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx) return;
    
    logging_printf( LOGGING_DEBUG, "Journal has data: %s\n", ( journal_has_data( ctx->journal ) ? "YES" : "NO" ) );
    
    if( ! journal_has_data( ctx->journal ) ) return;
    
    journal_dump( ctx->journal );
}

void net_ctx_journal_pack( uint8_t ctx_id, char **journal_buffer, size_t *journal_buffer_size)
{
    net_ctx_t *ctx = NULL;
    
    *journal_buffer = NULL;
    *journal_buffer_size = 0;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx) return;
    
    journal_pack( ctx->journal, journal_buffer, journal_buffer_size );
}

void net_ctx_journal_reset( uint8_t ctx_id )
{
    net_ctx_t *ctx = NULL;
    
    if( ctx_id > _max_ctx - 1) return;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx) return;
    
    journal_reset( ctx->journal);
}

void net_ctx_update_rtp_fields( uint8_t ctx_id, rtp_packet_t *rtp_packet)
{
    net_ctx_t *ctx = NULL;
    
    if( ! rtp_packet ) return;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx ) return;
    
    rtp_packet->header.seq = ctx->seq;
    rtp_packet->header.timestamp = time(0) - ctx->start;
    rtp_packet->header.ssrc = ctx->send_ssrc;
}

void net_ctx_increment_seq( uint8_t ctx_id )
{
    net_ctx_t *ctx = NULL;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx ) return;
    
    ctx->seq += 1;
}

void net_ctx_send( int send_socket, uint8_t ctx_id, unsigned char *buffer, size_t buffer_len )
{
    net_ctx_t *ctx = NULL;
    struct sockaddr_in send_address;
    ssize_t bytes_sent = 0;
    
    if( ! buffer ) return;
    if( buffer_len <= 0 ) return;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx ) return;
    
    debug_net_ctx_dump( ctx );
    debug_ctx_journal_dump( ctx_id );
    
    
    /* Set up the destination address */
    memset((char *)&send_address, 0, sizeof( send_address));
    send_address.sin_family = AF_INET;
    send_address.sin_port = htons( ctx->data_port ) ;
    inet_aton( ctx->ip_address, &send_address.sin_addr );
    
    bytes_sent = sendto( send_socket, buffer, buffer_len , 0 , (struct sockaddr *)&send_address, sizeof( send_address ) );
    
    if( bytes_sent < 0 )
    {
        logging_printf( LOGGING_ERROR, "Failed to send %u bytes to %s:%u\n%s\n", buffer_len, ctx->ip_address, ctx->data_port , strerror( errno ));
    } else {
        logging_printf( LOGGING_DEBUG, "Sent %u bytes to %s:%u\n", bytes_sent, ctx->ip_address, ctx->data_port );
    }
}

void net_ctx_send_local( int send_socket, unsigned char *buffer, size_t buffer_len )
{
    int ctx_id = 0;
    
    net_ctx_t *ctx = NULL;
    struct sockaddr_in send_address;
    ssize_t bytes_sent = 0;
    
    if( ! buffer ) return;
    if( buffer_len <= 0 ) return;
    
    ctx = net_ctx_find_by_id( ctx_id );
    
    if( ! ctx ) return;
    
    debug_net_ctx_dump( ctx );
    debug_ctx_journal_dump( ctx_id );
    
    // set ip adress to localhost and port
    unsigned char ipAdress_t[] = "127.0.0.1";
    unsigned char *ipAdress = &ipAdress_t[0];
    int port = 5010;
    
    /* Set up the destination address */
    memset((char *)&send_address, 0, sizeof( send_address));
    send_address.sin_family = AF_INET;
    send_address.sin_port = htons( port ) ;
    inet_aton( ipAdress, &send_address.sin_addr );
    
    //send message
    bytes_sent = sendto( send_socket, buffer, buffer_len , 0 , (struct sockaddr *)&send_address, sizeof( send_address ) );
    
    if( bytes_sent < 0 )
    {
        logging_printf( LOGGING_ERROR, "Failed to send %u bytes to %s:%u\n%s\n", buffer_len, ctx->ip_address, ctx->data_port , strerror( errno ));
    } else {
        logging_printf( LOGGING_DEBUG, "Sent %u bytes to %s:%u\n", bytes_sent, ctx->ip_address, ctx->data_port );
    }
}
