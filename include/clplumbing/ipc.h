/*
 * ipc.h IPC abstraction data structures.
 *
 * author  Xiaoxiang Liu <xiliu@ncsa.uiuc.edu>,
 *	Alan Robertson <alanr@unix.sh>
 *
 *
 * Copyright (c) 2002 International Business Machines
 * Copyright (c) 2002  Xiaoxiang Liu <xiliu@ncsa.uiuc.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef _IPC_H_
#define _IPC_H_
#include <glib.h>
#undef MIN
#undef MAX
#include <sys/types.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

/* constants */
#define DEFAULT_MAX_QLEN 20
#define MAX_MESSAGE_SIZE 4096

/* channel and connection status */
#define IPC_CONNECT 0
#define IPC_WAIT 1
#define IPC_DISCONNECT 2

/* general return values */
#define IPC_OK 0
#define IPC_FAIL 1
#define IPC_BROKEN 2
#define IPC_INTR 3

/*
 *	IPC:  Sockets-like Interprocess Communication Abstraction
 *
 *	We have two fundmental abstractions which we maintain.
 *	Everything else is in support of these two abstractions.
 *
 *	These two main abstractions are:
 *
 *	IPC_WaitConnection:
 *	A server-side abstraction for waiting for someone to connect.
 *
 *	IPC_Channel:
 *	An abstraction for an active communications channel.
 *
 *	All the operations on these two abstractions are carried out
 *	via function tables (channel->ops).  Below we refer to the
 *	function pointers in these tables as member functions.
 *
 *  On the server side, everything starts up with a call to
 *	ipc_wait_conn_constructor(), which returns an IPC_WaitConnection.
 *
 *	Once the server has the IPC_WaitConnection object in hand,
 *	it can give the result of the get_select_fd() member function
 *	to poll or select to inform you when someone tries to connect.
 *
 *	Once select tells you someone is trying to connect, you then
 *	use the accept_connection() member function to accept
 *	the connection.  accept_connection() returns an IPC_Channel.
 *
 *	With that, the server can talk to the client, and away they
 *	go ;-)
 *
 *  On the client side, everything starts up with a call to
 *	ipc_channel_constructor() which we use to talk to the server.
 *	The client is much easier ;-)
 */


typedef struct IPC_WAIT_CONNECTION	IPC_WaitConnection;
typedef struct IPC_CHANNEL		IPC_Channel;

typedef struct IPC_MESSAGE		IPC_Message;
typedef struct IPC_QUEUE		IPC_Queue;
typedef struct IPC_AUTH			IPC_Auth;

typedef struct IPC_OPS			IPC_Ops;
typedef struct IPC_WAIT_OPS		IPC_WaitOps;

/* wait connection structure. */
struct IPC_WAIT_CONNECTION{
	int		ch_status;	/* wait conn. status.*/
	void *		ch_private;	/* wait conn. private data. */
	IPC_WaitOps	*ops;		/* wait conn. function table .*/
};

/* channel structure.*/
struct IPC_CHANNEL{
	int		ch_status;	/* identify the status of channel.*/
	pid_t		farside_pid;	/* far side pid */
	void*		ch_private;	/* channel private data. */
					/* (may contain conn. info.) */
	IPC_Ops*	ops;		/* IPC_Channel function table.*/
/*
 *  There two queues in channel. One is for sending and the other
 *  is for receiving. 
 *  Those two queues are channel's internal queues. They should not be 
 *  accessed directly.
 */
/* private: */
	IPC_Queue*	send_queue; 
	IPC_Queue*	recv_queue; 

};

struct IPC_QUEUE{
	int		current_qlen;	/* Current qlen */
	int		max_qlen;	/* Max allowed qlen */
	GList*	queue;		/* List of messages */
};

/* authentication information : set of gids and uids */
struct IPC_AUTH {
	GHashTable * uid;	/* hash table for user id */
	GHashTable * gid;	/* hash table for group id */
};

/* Message structure. */
struct IPC_MESSAGE{
	unsigned long		msg_len;
	void*			msg_body;
/* 
 * IPC_MESSAGE::msg_done 
 *   the callback function pointer which can be called after this 
 *   message is sent, received or otherwise processed.
 *
 * Parameter:
 * msg: the back pointer to the message which contains this
 *	function pointer.
 * 
 */
	void (* msg_done)(IPC_Message * msg);
	void* msg_private;	/* the message private data.	*/
				/* Belongs to message creator	*/
				/* May be used by callback function. */
	IPC_Channel * msg_ch;	/* Channel the */
				/* the message is from/in */
};

struct IPC_WAIT_OPS{
/*
 * IPC_WAIT_OPS::destroy
 *   detroy the wait connection and free the memory space used by
 *	this wait connection.
 * 
 * Parameters:
 *   wait_conn (IN):  the pointer to the wait connection.
 *
 */ 
	void (* destroy)(IPC_WaitConnection *wait_conn);
/*
 * IPC_WAIT_OPS::get_select_fd
 *   provide a fd which user can listen on for a new coming connection.
 *
 * Parameters: 
 *   wait_conn (IN) : the pointer to the wait connection which
 *	we're supposed to return the file descriptor for
 *	(the file descriptor can be used with poll too ;-))
 *
 * Return values:
 *   integer >= 0 :  the select_fd.
 *       -1       :  can't get the select fd.
 *
 */
	int (* get_select_fd)(IPC_WaitConnection *wait_conn);
/*
 * IPC_WAIT_OPS::accept_connection
 *   accept and create a new connection and verify the authentication.
 *
 * Parameters:
 *   wait_conn (IN) : the waiting connection which will accept
 *	create the new connection.
 *   auth_info (IN) : the authentication information which will be
 *	verified for the new connection.
 *
 * Return values:
 *   the pointer to the new IPC channel; NULL if the creation or
 *	authentication fails.
 *
 */
	IPC_Channel * (* accept_connection)
		(IPC_WaitConnection * wait_conn, IPC_Auth *auth_info);
};

/* Standard IPC channel operations */

struct IPC_OPS{
/*
 * IPC_OPS::destroy
 *   brief destory the channel object.
 *
 * Parameters:
 *   ch  (IN) : the pointer to the channel which will be destroyed.
 *
 */
	void  (*destroy) (IPC_Channel * ch);
/*
 * IPC_OPS::initiate_connection
 *   used by service user side to set up a connection.
 *
 * Parameters:
 *   ch (IN) : the pointer to channel used to initiate the connection. 
 *
 * Return values:
 *   IPC_OK  : the channel set up the connection succesully.
 *   IPC_FAIL     : the connection initiation fails.
 *
 */
	int (* initiate_connection) (IPC_Channel * ch);
/*
 * IPC_OPS::verify_auth
 *   used by either side to verify the identity of peer on connection.
 *
 * Parameters
 *   ch (IN) : the pointer to the channel.
 *
 * Return values:
 *   IPC_OK   : the peer is trust.
 *   IPC_FAIL : verifying authentication fails.
 */
	int (* verify_auth) (IPC_Channel * ch, IPC_Auth* info);
/*
 * IPC_OPS::assert_auth
 *   service user asserts to be certain qualified service user.
 *
 * Parameters:
 *   ch    (IN):  the active channel.
 *   auth  (IN):  the hash table contain the asserting information.
 *
 * Return values:
 *   IPC_OK :  assert the authentication successfully.
 *   IPC_FAIL    : assertion fails.
 *
 * NOTE:  This operation is a bit obscure.  It isn't needed with
 *	UNIX domain sockets at all.  The intent is that some kinds
 *	of IPC (like FIFOs), do not have an intrinsic method to
 *	authenticate themselves except through file permissions.
 *	The idea is that you must tell it how to chown/grp your
 *	FIFO so that the other side and see that if you can write
 *	this, you can ONLY be the user/group they expect you to be.
 *	But, I think the parameters may be wrong for this ;-)
 */
	int (* assert_auth) (IPC_Channel * ch, GHashTable * auth);
/*
 * IPC_OPS::send
 *   send the message through the sending connection.
 *
 * Parameters:
 *   ch  (IN) : the channel which contains the connection.
 *   msg (IN) : pointer to the sending message. User must
 *	allocate the message space.
 *
 * Return values:
 *   IPC_OK : the message was either sent out successfully or
 *	appended to the send_queue.
 *   IPC_FAIL    : the send operation failed.
 *   IPC_BROKEN  : the channel is broken.
 *
*/    
	int (* send) (IPC_Channel * ch, IPC_Message* msg);

/*
 * IPC_OPS::recv
 *   receive the message through receving queue.
 *
 * Parameters:
 *   ch  (IN) : the channel which contains the connection.
 *   msg (OUT): the IPC_MESSAGE** pointer which contains the pointer
 *		to the recevied message or NULL if there is no
 *		message available.
 *
 * Return values:
 *   IPC_OK	: receive operation is completed successfully.
 *   IPC_FAIL	: operation failed.
 *   IPC_BROKEN	: the channel is broken (disconnected)
 *
 * Note: 
 *   return value IPC_OK doesn't mean the message is already 
 *   sent out to (or received by) the peer. It may be pending
 *   in the send_queue.  In order to make sure the message is no
 *   longer needed, please specify the msg_done function in the
 *   message structure and once this function is called, the
 *   message is no longer needed.
 *
 *   is_sending_blocked() is another way to check if there is a message 
 *   pending in the send_queue.
 *
 */
	int (* recv) (IPC_Channel * ch, IPC_Message** msg);

/*
 * IPC_OPS::waitin
 *   Wait for input to become available
 *
 * Parameters:
 *   ch  (IN) : the channel which contains the connection.
 *
 * Side effects:
 *	If output becomes unblocked while waiting, it will automatically
 *	be resumed without comment.
 *
 * Return values:
 *   IPC_OK	: a message is pending or output has become unblocked.
 *   IPC_FAIL	: operation failed.
 *   IPC_BROKEN	: the channel is broken (disconnected)
 *   IPC_INTR	: waiting was interrupted by a signal
 */
	int (* waitin) (IPC_Channel * ch);

/*
 * IPC_OPS::is_message_pending
 *   check to see if there is any messages ready to read, or hangup has
 *   occurred.
 *
 * Parameters:
 *   ch (IN) : the pointer to the channel.
 *
 * Return values:
 *   TRUE : there are messages ready to read, or hangup.
 *   FALSE: there are no messages ready to be read.
 */
	gboolean (* is_message_pending) (IPC_Channel  * ch);

/*
 * IPC_OPS::is_sending_blocked
 *   check the send_queue to see if there are any messages blocked. 
 *
 * Parameters:
 *   ch (IN) : the pointer to the channel.
 *
 * Return values:
 *   TRUE : there are messages blocked (waiting) in the send_queue.
 *   FALSE: there are no message blocked (waiting) in the send_queue.
 *
 *  See also:
 *	get_send_select_fd()
 */  
	gboolean (* is_sending_blocked) (IPC_Channel  *ch);

/*
 * IPC_OPS::resume_io
 *   Resume all possible IO operations through the IPC transport
 *
 * Parameters:
 *   the pointer to the channel.
 *
 * Return values:
 *   IPC_OK : resume all the possible I/O operation successfully.
 *   IPC_FAIL   : the operation fails.
 *   IPC_BROKEN : the channel is broken.
 *
 */
	int (* resume_io) (IPC_Channel  *ch);
/*
 * IPC_OPS::get_send_select_fd()
 *   return a file descriptor which can be given to select/poll. This fd
 *   is used by the IPC code for sending.  It is intended that this be
 *   ONLY used with select, poll, or similar mechanisms, not for direct I/O.
 *   Note that due to select(2) and poll(2) semantics, you must check
 *   is_sending_blocked() to see whether you should include this FD in
 *   your poll for writability, or you will loop very fast in your
 *   select/poll loop ;-)
 *
 * Parameters:
 *   ch (IN) : the pointer to the channel.
 *
 * Return values:
 *   integer >= 0 : the send fd for selection.
 *      -1         : there is no send fd.
 *
 *  See also:
 *	is_sending_blocked()
 */
	int   (* get_send_select_fd) (IPC_Channel * ch);
/*
 * IPC_OPS::get_recv_select_fd
 *   return a file descriptor which can be given to select. This fd
 *   is for receiving.  It is intended that this be ONLY used with select,
 *   poll, or similar mechanisms, NOT for direct I/O.
 *
 * Parameters:
 *   ch (IN) : the pointer to the channel.
 *
 * Return values:
 *   integer >= 0 : the recv fd for selection.
 *       -1        : there is no recv fd.
 *
 *	NOTE:  This file descriptor is often the same as the send
 *	file descriptor.
 */
	int   (* get_recv_select_fd) (IPC_Channel * ch);
/*
 * IPC_OPS::set_send_qlen
 *   allow user to set the maximum send_queue length.
 *
 * Parameters
 *   ch    (IN) : the pointer to the channel.
 *   q_len (IN) : the max length for the send_queue.
 *
 * Return values:
 *   IPC_OK : set the send queue length successfully.
 *   IPC_FAIL    : there is no send queue. (This isn't supposed
 *		 	to happen).
 *                It means something bad happened.
 *
 */
	int  (* set_send_qlen) (IPC_Channel * ch, int q_len);
/*
 * IPC_OPS::set_recv_qlen
 *   allow user to set the maximum recv_queue length.
 *
 * Parameters:
 *   ch    (IN) : the pointer to the channel.
 *   q_len (IN) : the max length for the recv_queue.
 *
 * Return values:
 *   IPC_OK : set the recv queue length successfully.
 *   IPC_FAIL    : there is no recv queue.
 *
 */
	int  (* set_recv_qlen) (IPC_Channel * ch, int q_len);
};


/*
 * ipc_wait_conn_constructor:
 *    the common constructor for ipc waiting connection. 
 *    Use ch_type to identify the connection type. Usually it's only
 *    needed by server side.
 *
 * Parameters:
 *    ch_type   (IN) : the type of the waiting connection to create.
 *    ch_attrs  (IN) : the hash table which contains the attributes
 *			needed by this waiting connection in name/value
 *			pair format.
 *
 *			For example, the only attribute needed by UNIX
 *			domain sockets is path name.
 *
 * Return values:
 *    the pointer to a new waiting connection or NULL if the connection
 *			can't be created.
 * Note:
 *    current implementation only supports unix domain socket 
 *    whose type is IPC_DOMAIN_SOCKET 
 *
 */
extern IPC_WaitConnection * ipc_wait_conn_constructor(const char * ch_type
,	GHashTable* ch_attrs);

/*
 * ipc_channel_constructor:
 *   brief the common constructor for ipc channel. 
 *   Use ch_type to identify the channel type.
 *   Usually this function is only called by client side.
 *
 * Parameters:
 *   ch_type  (IN): the type of the channel you want to create.
 *   ch_attrs (IN): the hash table which contains the attributes needed
 *		by this channel.
 *                  For example, the only attribute needed by UNIX domain
 *			socket is path name.
 *
 * Return values:
 *   the pointer to the new channel whose status is IPC_DISCONNECT
 *	or NULL if the channel can't be created.
 *
 * Note:
 *   current implementation only support unix domain socket 
 *   whose type is IPC_DOMAIN_SOCKET 
 *
 */
extern IPC_Channel  * ipc_channel_constructor(const char * ch_type
,	GHashTable* ch_attrs);

/*
 * ipc_channel_pair:
 *   Construct a pair of connected IPC channels in a fashion analagous
 *	to pipe(2) or socketpair(2).
 *
 * Parameters:
 *   channels: an array of two IPC_Channel pointers for return result
 */
int ipc_channel_pair(IPC_Channel* channels[2]);

/*
 * ipc_set_auth:
 *   A helper function used to convert array of uid and gid into
 *	an authentication structure (IPC_Auth)
 *
 * Parameters:
 *   a_uid    (IN): the array of a set of user ids.
 *   a_gid    (IN): the array of a set of group ids.
 *   num_uid  (IN): the number of user ids.
 *   num_gid  (IN): the number of group ids.
 *
 * Return values:
 *   the pointer to the authentication structure which contains the 
 *   set of uid and the set of gid. Or NULL if this structure can't
 *	be created.
 *
 */
extern IPC_Auth * ipc_set_auth(uid_t * a_uid, gid_t * a_gid
,	int num_uid, int num_gid);

/* Destroys an object constructed by ipc_set_auth */
extern void ipc_destroy_auth(IPC_Auth * auth);


#define	IPC_PATH_ATTR		"path"		/* pathname attribute */
#define	IPC_DOMAIN_SOCKET	"uds"		/* Unix domain socket */

#ifdef IPC_DOMAIN_SOCKET
#	define	IPC_ANYTYPE		IPC_DOMAIN_SOCKET
#else
#	error "No IPC types defined(!)"
#endif

#endif
