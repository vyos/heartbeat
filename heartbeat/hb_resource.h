#ifndef _HB_RESOURCE_H
#define _HB_RESOURCE_H
#include <heartbeat.h>
#include <heartbeat_private.h>
/* Resource-related types and variables */

enum standby { NOT, ME, OTHER, DONE };

enum hb_rsc_state {
	HB_R_INIT,		/* Links not up yet */
	HB_R_STARTING,		/* Links up, start message issued */
	HB_R_BOTHSTARTING,	/* Links up, start msg received & issued  */
				/* BOTHSTARTING now equiv to STARTING (?) */
	HB_R_RSCRCVD,		/* Resource Message received */
	HB_R_STABLE,		/* Local resources acquired, too... */
	HB_R_SHUTDOWN,		/* We're in shutdown... */
};

/*
 *	Note that the _RSC defines below are bit fields!
 */
#define HB_NO_RESOURCES		"none"
#define HB_NO_RSC			0

#define HB_LOCAL_RESOURCES		"local"
#define HB_LOCAL_RSC		1

#define HB_FOREIGN_RESOURCES	"foreign"
#define	HB_FOREIGN_RSC		2

#define HB_ALL_RSC		(HB_LOCAL_RSC|HB_FOREIGN_RSC)
#define HB_ALL_RESOURCES	"all"

typedef void	(*RemoteRscReqFunc)	(GHook *  data);

extern int		DoManageResources;
extern int 		nice_failback;
extern int		other_holds_resources;
extern int		other_is_stable;
extern int		takeover_in_progress;
extern enum hb_rsc_state resourcestate;
extern enum standby	going_standby;
extern longclock_t	standby_running;
extern longclock_t	local_takeover_time;
/* Also: procinfo->i_hold_resources */

/* Resource-related functions */

void	notify_world(struct ha_msg * msg, const char * ostatus);
void	PerformQueuedNotifyWorld(GHook* g);
int	parse_ha_resources(const char * cfgfile);
int 	encode_resources(const char *p);
const char * 	decode_resources(int);
int	send_local_starting(void);
void	process_resources(const char * type, struct ha_msg* msg
,		struct node_info * thisnode);
void	takeover_from_node(const char * nodename);
void	req_our_resources(int getthemanyway);
void	ask_for_resources(struct ha_msg *msg);
void AuditResources(void);
void	QueueRemoteRscReq(RemoteRscReqFunc, struct ha_msg* data);
void	hb_rsc_recover_dead_resources(struct node_info* hip);

#endif
