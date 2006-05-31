/* $Id: unpack.c,v 1.1 2006/05/31 14:59:12 andrew Exp $ */
/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <portability.h>
#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>
#include <clplumbing/cl_misc.h>

#include <lrm/lrm_api.h>

#include <glib.h>

#include <heartbeat.h> /* for ONLINESTATUS */

#include <lib/crm/pengine/status.h>
#include <utils.h>
#include <lib/crm/pengine/rules.h>

gint sort_op_by_callid(gconstpointer a, gconstpointer b);

gboolean unpack_rsc_to_attr(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_to_node(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_order(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_colocation(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_location(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_lrm_resources(
	node_t *node, crm_data_t * lrm_state, pe_working_set_t *data_set);

gboolean add_node_attrs(
	crm_data_t * attrs, node_t *node, pe_working_set_t *data_set);

gboolean unpack_rsc_op(
	resource_t *rsc, node_t *node, crm_data_t *xml_op,
	int *max_call_id, enum action_fail_response *failed, pe_working_set_t *data_set);

gboolean determine_online_status(
	crm_data_t * node_state, node_t *this_node, pe_working_set_t *data_set);

gboolean rsc_colocation_new(
	const char *id, enum con_strength strength,
	resource_t *rsc_lh, resource_t *rsc_rh,
	const char *state_lh, const char *state_rh);

gboolean create_ordering(
	const char *id, enum con_strength strength,
	resource_t *rsc_lh, resource_t *rsc_rh, pe_working_set_t *data_set);

const char *param_value(
	GHashTable *hash, crm_data_t * parent, const char *name);

rsc_to_node_t *generate_location_rule(
	resource_t *rsc, crm_data_t *location_rule, pe_working_set_t *data_set);

#define get_cluster_pref(pref) value = g_hash_table_lookup(config_hash, pref); \
	if(value == NULL) {						\
		pe_config_warn("No value specified for cluster preference: %s", pref); \
	}


gboolean
unpack_config(crm_data_t * config, pe_working_set_t *data_set)
{
	const char *name = NULL;
	const char *value = NULL;
	GHashTable *config_hash = g_hash_table_new_full(
		g_str_hash,g_str_equal, g_hash_destroy_str,g_hash_destroy_str);

	data_set->config_hash = config_hash;	

	unpack_instance_attributes(
		config, XML_CIB_TAG_PROPSET, NULL, config_hash,
		CIB_OPTIONS_FIRST, data_set->now);

#if CRM_DEPRECATED_SINCE_2_0_1
	xml_child_iter_filter(
		config, a_child, XML_CIB_TAG_NVPAIR,

		name = crm_element_value(a_child, XML_NVPAIR_ATTR_NAME);

		value = crm_element_value(a_child, XML_NVPAIR_ATTR_VALUE);
		if(g_hash_table_lookup(config_hash, name) == NULL) {
			g_hash_table_insert(
				config_hash,crm_strdup(name),crm_strdup(value));
		}
		pe_config_err("Creating <nvpair id=%s name=%s/> directly"
			      "beneath <crm_config> has been depreciated since"
			      " 2.0.1", ID(a_child), name);
		);
#else
	xml_child_iter_filter(
		config, a_child, XML_CIB_TAG_NVPAIR,

		name = crm_element_value(a_child, XML_NVPAIR_ATTR_NAME);
		pe_config_err("Creating <nvpair id=%s name=%s/> directly"
			      "beneath <crm_config> has been depreciated since"
			      " 2.0.1 and is now disabled", ID(a_child), name);
		);
#endif
	
	get_cluster_pref("transition_idle_timeout");
	if(value != NULL) {
		long tmp = crm_get_msec(value);
		if(tmp > 0) {
			crm_free(data_set->transition_idle_timeout);
			data_set->transition_idle_timeout = crm_strdup(value);
		} else {
			crm_err("Invalid value for transition_idle_timeout: %s",
				value);
		}
	}
	
	crm_debug("%s set to: %s",
		 "transition_idle_timeout", data_set->transition_idle_timeout);

	get_cluster_pref("default_"XML_RSC_ATTR_STICKINESS);
	data_set->default_resource_stickiness = char2score(value);
	crm_info("Default stickiness: %d",
		 data_set->default_resource_stickiness);

	get_cluster_pref("default_"XML_RSC_ATTR_FAIL_STICKINESS);
	data_set->default_resource_fail_stickiness = char2score(value);
	crm_info("Default failure stickiness: %d",
		 data_set->default_resource_fail_stickiness);
	
	get_cluster_pref("stonith_enabled");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->stonith_enabled);
	}
	crm_info("STONITH of failed nodes is %s",
		 data_set->stonith_enabled?"enabled":"disabled");	

	get_cluster_pref("stonith_action");
	if(value == NULL || safe_str_neq(value, "poweroff")) {
		value = "reboot";
	}
	data_set->stonith_action = value;
	crm_info("STONITH will %s nodes", data_set->stonith_action);	
	
	get_cluster_pref("symmetric_cluster");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->symmetric_cluster);
	}
	if(data_set->symmetric_cluster) {
		crm_info("Cluster is symmetric"
			 " - resources can run anywhere by default");
	}

	get_cluster_pref("no_quorum_policy");
	if(safe_str_eq(value, "ignore")) {
		data_set->no_quorum_policy = no_quorum_ignore;
		
	} else if(safe_str_eq(value, "freeze")) {
		data_set->no_quorum_policy = no_quorum_freeze;

	} else {
		data_set->no_quorum_policy = no_quorum_stop;
	}
	
	switch (data_set->no_quorum_policy) {
		case no_quorum_freeze:
			crm_info("On loss of CCM Quorum: Freeze resources");
			break;
		case no_quorum_stop:
			crm_info("On loss of CCM Quorum: Stop ALL resources");
			break;
		case no_quorum_ignore:
			crm_notice("On loss of CCM Quorum: Ignore");
			break;
	}

	get_cluster_pref("stop_orphan_resources");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->stop_rsc_orphans);
	}
	crm_info("Orphan resources are %s",
		 data_set->stop_rsc_orphans?"stopped":"ignored");	
	
	get_cluster_pref("stop_orphan_actions");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->stop_action_orphans);
	}
	crm_info("Orphan resource actions are %s",
		 data_set->stop_action_orphans?"stopped":"ignored");	

	get_cluster_pref("remove_after_stop");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->remove_after_stop);
	}
	crm_info("Stopped resources are removed from the status section: %s",
		 data_set->remove_after_stop?"true":"false");	
	
	get_cluster_pref("is_managed_default");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->is_managed_default);
	}
	crm_info("By default resources are %smanaged",
		 data_set->is_managed_default?"":"not ");

	return TRUE;
}

gboolean
unpack_nodes(crm_data_t * xml_nodes, pe_working_set_t *data_set)
{
	node_t *new_node   = NULL;
	const char *id     = NULL;
	const char *uname  = NULL;
	const char *type   = NULL;

	crm_debug_2("Begining unpack... %s",
		    xml_nodes?crm_element_name(xml_nodes):"<none>");
	xml_child_iter_filter(
		xml_nodes, xml_obj, XML_CIB_TAG_NODE,

		new_node = NULL;

		id     = crm_element_value(xml_obj, XML_ATTR_ID);
		uname  = crm_element_value(xml_obj, XML_ATTR_UNAME);
		type   = crm_element_value(xml_obj, XML_ATTR_TYPE);
		crm_debug_3("Processing node %s/%s", uname, id);

		if(id == NULL) {
			pe_config_err("Must specify id tag in <node>");
			continue;
		}
		if(type == NULL) {
			pe_config_err("Must specify type tag in <node>");
			continue;
		}
		crm_malloc0(new_node, sizeof(node_t));
		if(new_node == NULL) {
			return FALSE;
		}
		
		new_node->weight = 0;
		new_node->fixed  = FALSE;
		crm_malloc0(new_node->details,
			   sizeof(struct node_shared_s));

		if(new_node->details == NULL) {
			crm_free(new_node);
			return FALSE;
		}

		crm_debug_3("Creaing node for entry %s/%s", uname, id);
		new_node->details->id		= id;
		new_node->details->uname	= uname;
		new_node->details->type		= node_ping;
		new_node->details->online	= FALSE;
		new_node->details->shutdown	= FALSE;
		new_node->details->running_rsc	= NULL;
		new_node->details->attrs        = g_hash_table_new_full(
			g_str_hash, g_str_equal,
			g_hash_destroy_str, g_hash_destroy_str);
		
/* 		if(data_set->have_quorum == FALSE */
/* 		   && data_set->no_quorum_policy == no_quorum_stop) { */
/* 			/\* start shutting resources down *\/ */
/* 			new_node->weight = -INFINITY; */
/* 		} */
		
		if(data_set->stonith_enabled) {
			/* all nodes are unclean until we've seen their
			 * status entry
			 */
			new_node->details->unclean = TRUE;
		} else {
			/* blind faith... */
			new_node->details->unclean = FALSE; 
		}
		
		
		if(type == NULL
		   || safe_str_eq(type, "member")
		   || safe_str_eq(type, NORMALNODE)) {
			new_node->details->type = node_member;
		}

		add_node_attrs(xml_obj, new_node, data_set);

		if(crm_is_true(g_hash_table_lookup(
				       new_node->details->attrs, "standby"))) {
			crm_info("Node %s is in standby-mode",
				 new_node->details->uname);
			new_node->weight = -INFINITY;
			new_node->details->standby = TRUE;
		}
		
		data_set->nodes = g_list_append(data_set->nodes, new_node);    
		crm_debug_3("Done with node %s",
			    crm_element_value(xml_obj, XML_ATTR_UNAME));

		crm_action_debug_3(print_node("Added", new_node, FALSE));
		);
  
/* 	data_set->nodes = g_list_sort(data_set->nodes, sort_node_weight); */

	return TRUE;
}

gboolean 
unpack_resources(crm_data_t * xml_resources, pe_working_set_t *data_set)
{
	crm_debug_2("Begining unpack... %s",
		    xml_resources?crm_element_name(xml_resources):"<none>");
	xml_child_iter(
		xml_resources, xml_obj, 

		resource_t *new_rsc = NULL;
		crm_debug_2("Begining unpack... %s",
			    xml_obj?crm_element_name(xml_obj):"<none>");
		if(common_unpack(xml_obj, &new_rsc, NULL, data_set)) {
			data_set->resources = g_list_append(
				data_set->resources, new_rsc);
			
			print_resource(LOG_DEBUG_3, "Added", new_rsc, FALSE);

		} else {
			pe_config_err("Failed unpacking %s %s",
				      crm_element_name(xml_obj),
				      crm_element_value(xml_obj, XML_ATTR_ID));
			if(new_rsc != NULL && new_rsc->fns != NULL) {
				new_rsc->fns->free(new_rsc);
			}
		}
		);
	
	data_set->resources = g_list_sort(
		data_set->resources, sort_rsc_priority);

	return TRUE;
}

gboolean 
unpack_constraints(crm_data_t * xml_constraints, pe_working_set_t *data_set)
{
	crm_data_t *lifetime = NULL;
	crm_debug_2("Begining unpack... %s",
		    xml_constraints?crm_element_name(xml_constraints):"<none>");
	xml_child_iter(
		xml_constraints, xml_obj, 

		const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
		if(id == NULL) {
			pe_config_err("Constraint <%s...> must have an id",
				crm_element_name(xml_obj));
			continue;
		}

		crm_debug_3("Processing constraint %s %s",
			    crm_element_name(xml_obj),id);

		lifetime = cl_get_struct(xml_obj, "lifetime");

		if(test_ruleset(lifetime, NULL, data_set->now) == FALSE) {
			crm_info("Constraint %s %s is not active",
				 crm_element_name(xml_obj), id);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_ORDER,
				      crm_element_name(xml_obj))) {
			unpack_rsc_order(xml_obj, data_set);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_DEPEND,
				      crm_element_name(xml_obj))) {
			unpack_rsc_colocation(xml_obj, data_set);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_LOCATION,
				      crm_element_name(xml_obj))) {
			unpack_rsc_location(xml_obj, data_set);

		} else {
			pe_err("Unsupported constraint type: %s",
				crm_element_name(xml_obj));
		}
		);

	return TRUE;
}

/* remove nodes that are down, stopping */
/* create +ve rsc_to_node constraints between resources and the nodes they are running on */
/* anything else? */
gboolean
unpack_status(crm_data_t * status, pe_working_set_t *data_set)
{
	const char *id    = NULL;
	const char *uname = NULL;

	crm_data_t * lrm_rsc    = NULL;
	crm_data_t * attrs      = NULL;
	node_t    *this_node  = NULL;
	
	crm_debug_3("Begining unpack");
	xml_child_iter_filter(
		status, node_state, XML_CIB_TAG_STATE,

		id         = crm_element_value(node_state, XML_ATTR_ID);
		uname = crm_element_value(node_state,    XML_ATTR_UNAME);
		attrs = find_xml_node(
			node_state, XML_TAG_TRANSIENT_NODEATTRS, FALSE);

		lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
		lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);

		crm_debug_3("Processing node %s", uname);
		this_node = pe_find_node_id(data_set->nodes, id);

		if(uname == NULL) {
			/* error */
			continue;

		} else if(this_node == NULL) {
			pe_config_warn("Node %s in status section no longer exists",
				       uname);
			continue;
		}

		/* Mark the node as provisionally clean
		 * - at least we have seen it in the current cluster's lifetime
		 */
		this_node->details->unclean = FALSE;
		
		crm_debug_3("Adding runtime node attrs");
		add_node_attrs(attrs, this_node, data_set);

		crm_debug_3("determining node state");
		determine_online_status(node_state, this_node, data_set);

		if(this_node->details->online || data_set->stonith_enabled) {
			/* offline nodes run no resources...
			 * unless stonith is enabled in which case we need to
			 *   make sure rsc start events happen after the stonith
			 */
			crm_debug_3("Processing lrm resource entries");
			unpack_lrm_resources(this_node, lrm_rsc, data_set);
		}
		);

	return TRUE;
	
}

static gboolean
determine_online_status_no_fencing(crm_data_t * node_state, node_t *this_node)
{
	gboolean online = FALSE;
	const char *join_state = crm_element_value(node_state,
						   XML_CIB_ATTR_JOINSTATE);
	const char *crm_state  = crm_element_value(node_state,
						   XML_CIB_ATTR_CRMDSTATE);
	const char *ccm_state  = crm_element_value(node_state,
						   XML_CIB_ATTR_INCCM);
	const char *ha_state   = crm_element_value(node_state,
						   XML_CIB_ATTR_HASTATE);
	const char *exp_state  = crm_element_value(node_state,
						   XML_CIB_ATTR_EXPSTATE);

	if(!crm_is_true(ccm_state) || safe_str_eq(ha_state,DEADSTATUS)){
		crm_debug_2("Node is down: ha_state=%s, ccm_state=%s",
			    crm_str(ha_state), crm_str(ccm_state));
		
	} else if(!crm_is_true(ccm_state)
		  || safe_str_eq(ha_state, DEADSTATUS)) {
		
	} else if(safe_str_neq(join_state, CRMD_JOINSTATE_DOWN)
		  && safe_str_eq(crm_state, ONLINESTATUS)) {
		online = TRUE;
		
	} else if(this_node->details->expected_up == FALSE) {
		crm_debug_2("CRMd is down: ha_state=%s, ccm_state=%s",
			    crm_str(ha_state), crm_str(ccm_state));
		crm_debug_2("\tcrm_state=%s, join_state=%s, expected=%s",
			    crm_str(crm_state), crm_str(join_state),
			    crm_str(exp_state));
		
	} else {
		/* mark it unclean */
		this_node->details->unclean = TRUE;
		
		crm_warn("Node %s is partially & un-expectedly down",
			 this_node->details->uname);
		crm_info("\tha_state=%s, ccm_state=%s,"
			 " crm_state=%s, join_state=%s, expected=%s",
			 crm_str(ha_state), crm_str(ccm_state),
			 crm_str(crm_state), crm_str(join_state),
			 crm_str(exp_state));
	}
	return online;
}

static gboolean
determine_online_status_fencing(crm_data_t * node_state, node_t *this_node)
{
	gboolean online = FALSE;
	const char *join_state = crm_element_value(node_state,
						   XML_CIB_ATTR_JOINSTATE);
	const char *crm_state  = crm_element_value(node_state,
						   XML_CIB_ATTR_CRMDSTATE);
	const char *ccm_state  = crm_element_value(node_state,
						   XML_CIB_ATTR_INCCM);
	const char *ha_state   = crm_element_value(node_state,
						   XML_CIB_ATTR_HASTATE);
	const char *exp_state  = crm_element_value(node_state,
						   XML_CIB_ATTR_EXPSTATE);

	if(crm_is_true(ccm_state)
	   && (ha_state == NULL || safe_str_eq(ha_state, ACTIVESTATUS))
	   && safe_str_eq(crm_state, ONLINESTATUS)
	   && safe_str_eq(join_state, CRMD_JOINSTATE_MEMBER)) {
		online = TRUE;
		
	} else if(crm_is_true(ccm_state) == FALSE
/* 		  && safe_str_eq(ha_state, DEADSTATUS) */
		  && safe_str_eq(crm_state, OFFLINESTATUS)
		  && this_node->details->expected_up == FALSE) {
		crm_debug("Node %s is down: join_state=%s, expected=%s",
			  this_node->details->uname,
			  crm_str(join_state), crm_str(exp_state));
		
	} else if(this_node->details->expected_up == FALSE) {
		crm_info("Node %s is comming up", this_node->details->uname);
		crm_debug("\tha_state=%s, ccm_state=%s,"
			  " crm_state=%s, join_state=%s, expected=%s",
			  crm_str(ha_state), crm_str(ccm_state),
			  crm_str(crm_state), crm_str(join_state),
			  crm_str(exp_state));

	} else {
		/* mark it unclean */
		this_node->details->unclean = TRUE;
		
		crm_warn("Node %s (%s) is un-expectedly down",
			 this_node->details->uname, this_node->details->id);
		crm_info("\tha_state=%s, ccm_state=%s,"
			 " crm_state=%s, join_state=%s, expected=%s",
			 crm_str(ha_state), crm_str(ccm_state),
			 crm_str(crm_state), crm_str(join_state),
			 crm_str(exp_state));
	}
	return online;
}

gboolean
determine_online_status(
	crm_data_t * node_state, node_t *this_node, pe_working_set_t *data_set)
{
	int shutdown = 0;
	gboolean online = FALSE;
	const char *exp_state  =
		crm_element_value(node_state, XML_CIB_ATTR_EXPSTATE);
	
	if(this_node == NULL) {
		pe_config_err("No node to check");
		return online;
	}

	ha_msg_value_int(node_state, XML_CIB_ATTR_SHUTDOWN, &shutdown);
	
	this_node->details->expected_up = FALSE;
	if(safe_str_eq(exp_state, CRMD_JOINSTATE_MEMBER)) {
		this_node->details->expected_up = TRUE;
	}

	this_node->details->shutdown = FALSE;
	if(shutdown != 0) {
		this_node->details->shutdown = TRUE;
		this_node->details->expected_up = FALSE;
	}

	if(data_set->stonith_enabled == FALSE) {
		online = determine_online_status_no_fencing(
			node_state, this_node);
		
	} else {
		online = determine_online_status_fencing(
			node_state, this_node);
	}
	
	if(online) {
		this_node->details->online = TRUE;

	} else {
		/* remove node from contention */
		this_node->fixed = TRUE;
		this_node->weight = -INFINITY;
	}

	if(online && this_node->details->shutdown) {
		/* dont run resources here */
		this_node->fixed = TRUE;
		this_node->weight = -INFINITY;
	}	

	if(this_node->details->unclean) {
		pe_proc_warn("Node %s is unclean", this_node->details->uname);

	} else if(this_node->details->online) {
		crm_info("Node %s is %s", this_node->details->uname,
			 this_node->details->shutdown?"shutting down":"online");

	} else {
		crm_debug_2("Node %s is offline", this_node->details->uname);
	}
	
	

	return online;
}

#define set_char(x) last_rsc_id[len] = x; complete = TRUE;

static void
increment_clone(char *last_rsc_id)
{
	gboolean complete = FALSE;
	int len = 0;

	CRM_CHECK(last_rsc_id != NULL, return);
	if(last_rsc_id != NULL) {
		len = strlen(last_rsc_id);
	}
	len--;
	while(complete == FALSE && len > 0) {
		switch (last_rsc_id[len]) {
			case 0:
				len--;
				break;
			case '0':
				set_char('1');
				break;
			case '1':
				set_char('2');
				break;
			case '2':
				set_char('3');
				break;
			case '3':
				set_char('4');
				break;
			case '4':
				set_char('5');
				break;
			case '5':
				set_char('6');
				break;
			case '6':
				set_char('7');
				break;
			case '7':
				set_char('8');
				break;
			case '8':
				set_char('9');
				break;
			case '9':
				last_rsc_id[len] = '0';
				len--;
				break;
			default:
				crm_err("Unexpected char: %c (%d)",
					last_rsc_id[len], len);
				break;
		}
	}
}

extern gboolean DeleteRsc(resource_t *rsc, node_t *node, pe_working_set_t *data_set);

static resource_t *
unpack_find_resource(
	pe_working_set_t *data_set, node_t *node, const char *rsc_id)
{
	resource_t *rsc = NULL;
	gboolean is_duped_clone = FALSE;
	char *alt_rsc_id = crm_strdup(rsc_id);
	
	while(rsc == NULL) {
		crm_debug_3("looking for: %s", alt_rsc_id);
		rsc = pe_find_resource(data_set->resources, alt_rsc_id);
		/* no match */
		if(rsc == NULL) {
			crm_debug_3("not found");
			break;
			
			/* not running anywhere else */
		} else if(rsc->running_on == NULL) {
			crm_debug_3("not active yet");
			break;
			
			/* always unique */
		} else if(rsc->globally_unique) {
			crm_debug_3("unique");
			break;
			
			/* running somewhere already but we dont care
			 *   find another clone instead
			 */
		} else {
			crm_debug_2("find another one");
			rsc = NULL;
			is_duped_clone = TRUE;
			increment_clone(alt_rsc_id);
		}
	}
	crm_free(alt_rsc_id);
	if(is_duped_clone && rsc != NULL) {
		crm_info("Internally renamed %s on %s to %s",
			 rsc_id, node->details->uname, rsc->id);
/* 		rsc->name = rsc_id; */
	}
	return rsc;
}

static resource_t *
process_orphan_resource(crm_data_t *rsc_entry, node_t *node, pe_working_set_t *data_set) 
{
	resource_t *rsc = NULL;
	gboolean is_duped_clone = FALSE;
	const char *rsc_id   = crm_element_value(rsc_entry, XML_ATTR_ID);
	crm_data_t *xml_rsc  = create_xml_node(NULL, XML_CIB_TAG_RESOURCE);
	
	crm_log_xml_info(rsc_entry, "Orphan resource");
	
	pe_config_warn("Nothing known about resource %s running on %s",
		       rsc_id, node->details->uname);

	if(pe_find_resource(data_set->resources, rsc_id) != NULL) {
		is_duped_clone = TRUE;
	}
	
	copy_in_properties(xml_rsc, rsc_entry);
	
	common_unpack(xml_rsc, &rsc, NULL, data_set);
	rsc->orphan = TRUE;
	
	data_set->resources = g_list_append(data_set->resources, rsc);
	
	if(data_set->stop_rsc_orphans == FALSE && is_duped_clone == FALSE) {
		rsc->is_managed = FALSE;
		
	} else {
		crm_info("Making sure orphan %s is stopped", rsc_id);
		
		print_resource(LOG_DEBUG_3, "Added orphan", rsc, FALSE);
			
		CRM_CHECK(rsc != NULL, return NULL);
		slist_iter(
			any_node, node_t, data_set->nodes, lpc,
			rsc2node_new(
				"__orphan_dont_run__", rsc,
				-INFINITY, any_node, data_set);
			);
	}
	return rsc;
}

static gboolean
check_rsc_parameters(resource_t *rsc, node_t *node, crm_data_t *rsc_entry,
		     pe_working_set_t *data_set) 
{
	int attr_lpc = 0;
	gboolean force_restart = FALSE;
	gboolean delete_resource = FALSE;
	
	const char *value = NULL;
	const char *old_value = NULL;
	const char *attr_list[] = {
		XML_ATTR_TYPE, 
		XML_AGENT_ATTR_CLASS,
 		XML_AGENT_ATTR_PROVIDER
	};

	for(; attr_lpc < DIMOF(attr_list); attr_lpc++) {
		value = crm_element_value(rsc->xml, attr_list[attr_lpc]);
		old_value = crm_element_value(rsc_entry, attr_list[attr_lpc]);
		if(safe_str_eq(value, old_value)) {
			continue;
		}
		
		force_restart = TRUE;
		crm_notice("Forcing restart of %s on %s, %s changed: %s -> %s",
			   rsc->id, node->details->uname, attr_list[attr_lpc],
			   crm_str(old_value), crm_str(value));
	}
	if(force_restart) {
		/* make sure the restart happens */
		stop_action(rsc, node, FALSE);
		rsc->start_pending = TRUE;
		delete_resource = TRUE;
	}
	return delete_resource;
}

static void
process_rsc_state(resource_t *rsc, node_t *node,
		  enum action_fail_response on_fail,
		  pe_working_set_t *data_set) 
{
	crm_debug_2("Resource %s is %s on %s",
		    rsc->id, role2text(rsc->role),
		    node->details->uname);

	rsc->known_on = g_list_append(rsc->known_on, node);

	if(rsc->role != RSC_ROLE_STOPPED) { 
		if(on_fail != action_fail_ignore) {
			rsc->failed = TRUE;
			crm_debug_2("Force stop");
		}

		crm_debug_2("Adding %s to %s",
			    rsc->id, node->details->uname);
		native_add_running(rsc, node, data_set);
			
		if(on_fail == action_fail_ignore) {
			/* nothing to do */
		} else if(node->details->unclean) {
			stop_action(rsc, node, FALSE);

		} else if(on_fail == action_fail_fence) {
			/* treat it as if it is still running
			 * but also mark the node as unclean
			 */
			node->details->unclean = TRUE;
			stop_action(rsc, node, FALSE);
				
		} else if(on_fail == action_fail_block) {
			/* is_managed == FALSE will prevent any
			 * actions being sent for the resource
			 */
			rsc->is_managed = FALSE;
				
		} else if(on_fail == action_fail_migrate) {
			stop_action(rsc, node, FALSE);

			/* make sure it comes up somewhere else
			 * or not at all
			 */
			rsc2node_new("__action_migration_auto__",
				     rsc, -INFINITY, node, data_set);
				
		} else {
			stop_action(rsc, node, FALSE);
		}
			
	} else {
		char *key = stop_key(rsc);
		GListPtr possible_matches = find_actions(rsc->actions, key, node);
		slist_iter(stop, action_t, possible_matches, lpc,
			   stop->optional = TRUE;
			);
		crm_free(key);
			
/* 			if(rsc->failed == FALSE && node->details->online) { */
/* 				delete_resource = TRUE; */
/* 			}			 */
	}
}

static const char *
get_interval(crm_data_t *xml_op) 
{
	const char *interval_s = NULL;
        interval_s  = crm_element_value(xml_op, XML_LRM_ATTR_INTERVAL);
#if CRM_DEPRECATED_SINCE_2_0_4
	if(interval_s == NULL) {
		crm_data_t *params = NULL;
		params = find_xml_node(xml_op, XML_TAG_PARAMS, FALSE);
		if(params != NULL) {
			interval_s = crm_element_value(
				params, XML_LRM_ATTR_INTERVAL);
		}
	}
#endif
	
	CRM_CHECK(interval_s != NULL,
		  crm_err("Invalid rsc op: %s", ID(xml_op)); return "0");
	
	return interval_s;
}

static void
unpack_lrm_rsc_state(
	node_t *node, crm_data_t * rsc_entry, pe_working_set_t *data_set)
{
	int fail_count = 0;
	char *fail_attr = NULL;
	const char *value = NULL;
	const char *fail_val = NULL;
	gboolean delete_resource = FALSE;

	const char *rsc_id    = crm_element_value(rsc_entry, XML_ATTR_ID);

	int max_call_id = -1;
	GListPtr op_list = NULL;
	GListPtr sorted_op_list = NULL;

	enum action_fail_response on_fail = FALSE;
	enum rsc_role_e saved_role = RSC_ROLE_UNKNOWN;
	
	resource_t *rsc = unpack_find_resource(data_set, node, rsc_id);
	
	crm_debug_3("[%s] Processing %s on %s",
		    crm_element_name(rsc_entry), rsc_id, node->details->uname);
	
	if(rsc == NULL) {
		rsc = process_orphan_resource(rsc_entry, node, data_set);
	} 
	CRM_ASSERT(rsc != NULL);
	
	delete_resource = check_rsc_parameters(rsc, node, rsc_entry, data_set);

	/* process failure stickiness */
	fail_count = 0;
	fail_attr = crm_concat("fail-count", rsc->id, '-');
	fail_val = g_hash_table_lookup(node->details->attrs, fail_attr);
	if(fail_val != NULL) {
		crm_debug("%s: %s", fail_attr, fail_val);
		fail_count = crm_parse_int(fail_val, "0");
	}
	crm_free(fail_attr);
	if(fail_count > 0 && rsc->fail_stickiness != 0) {
		rsc2node_new("fail_stickiness", rsc,
			     fail_count * rsc->fail_stickiness,
			     node, data_set);
		crm_debug("Setting failure stickiness for %s on %s: %d",
			  rsc->id, node->details->uname,
			  fail_count * rsc->fail_stickiness);
	}

	/* process operations */
	max_call_id = -1;

	op_list = NULL;
	sorted_op_list = NULL;
		
	xml_child_iter_filter(
		rsc_entry, rsc_op, XML_LRM_TAG_RSC_OP,
		op_list = g_list_append(op_list, rsc_op);
		);

	if(op_list != NULL) {
		int stop_index = -1;
		int start_index = -1;
		const char *task = NULL;
		const char *status = NULL;
		saved_role = rsc->role;
		on_fail = action_fail_ignore;
		rsc->role = RSC_ROLE_STOPPED;
		sorted_op_list = g_list_sort(op_list, sort_op_by_callid);

		slist_iter(
			rsc_op, crm_data_t, sorted_op_list, lpc,
			task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
			status = crm_element_value(rsc_op, XML_LRM_ATTR_OPSTATUS);
			if(safe_str_eq(task, CRMD_ACTION_STOP)
			   && safe_str_eq(status, "0")) {
				stop_index = lpc;

			} else if(safe_str_eq(task, CRMD_ACTION_START)) {
				start_index = lpc;

			} else if(start_index <= stop_index
				  && safe_str_eq(task, CRMD_ACTION_STATUS)) {
				const char *rc = crm_element_value(rsc_op, XML_LRM_ATTR_RC);
				if(safe_str_eq(rc, "0")
				   || safe_str_eq(rc, "8")) {
					start_index = lpc;
				}
			}
			
			unpack_rsc_op(rsc, node, rsc_op,
				      &max_call_id, &on_fail, data_set);
			);

		crm_debug_2("%s: Start index %d, stop index = %d",
			    rsc->id, start_index, stop_index);
		slist_iter(rsc_op, crm_data_t, sorted_op_list, lpc,
			   int interval = 0;
			   char *key = NULL;
			   const char *id = ID(rsc_op);
			   const char *interval_s = NULL;
			   if(node->details->online == FALSE) {
				   crm_debug_4("Skipping %s/%s: node is offline",
					       rsc->id, node->details->uname);
				   break;
				   
			   } else if(start_index < stop_index) {
				   crm_debug_4("Skipping %s/%s: not active",
					       rsc->id, node->details->uname);
				   break;
				   
			   } else if(lpc <= start_index) {
				   crm_debug_4("Skipping %s/%s: old",
					       id, node->details->uname);
				   continue;
			   }
			   
			   interval_s = get_interval(rsc_op);
			   interval = crm_parse_int(interval_s, "0");
			   if(interval == 0) {
				   crm_debug_4("Skipping %s/%s: non-recurring",
					       id, node->details->uname);
				   continue;
			   }

			   status = crm_element_value(rsc_op, XML_LRM_ATTR_OPSTATUS);
			   if(safe_str_eq(status, "-1")) {
				   crm_debug_4("Skipping %s/%s: status",
					       id, node->details->uname);
				   continue;
			   }
			   task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
			   /* create the action */
			   key = generate_op_key(rsc->id, task, interval);
			   crm_debug_3("Creating %s/%s", key, node->details->uname);
			   custom_action(rsc, key, task, node,
					 TRUE, TRUE, data_set);
			);
		
		/* no need to free the contents */
		g_list_free(sorted_op_list);
		
		process_rsc_state(rsc, node, on_fail, data_set);
	}
	
	value = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_TARGET_ROLE);
	if(value != NULL && safe_str_neq("default", value)) {
		enum rsc_role_e req_role = text2role(value);
		if(req_role != RSC_ROLE_UNKNOWN && req_role != rsc->next_role){
			crm_debug("%s: Overwriting calculated next role %s"
				  " with requested next role %s",
				  rsc->id, role2text(rsc->next_role),
				  role2text(req_role));
			rsc->next_role = req_role;
		}
	}

	if(delete_resource) {
		DeleteRsc(rsc, node, data_set);
	}
		
	if(saved_role > rsc->role) {
		rsc->role = saved_role;
	}
}

gboolean
unpack_lrm_resources(node_t *node, crm_data_t * lrm_rsc_list, pe_working_set_t *data_set)
{
	CRM_CHECK(node != NULL, return FALSE);

	crm_debug_3("Unpacking resources on %s", node->details->uname);
	
	xml_child_iter_filter(
		lrm_rsc_list, rsc_entry, XML_LRM_TAG_RESOURCE,
		unpack_lrm_rsc_state(node, rsc_entry, data_set);
		);
	
	return TRUE;
}

#define sort_return(an_int) crm_free(a_uuid); crm_free(b_uuid); return an_int

gint
sort_op_by_callid(gconstpointer a, gconstpointer b)
{
	char *a_uuid = NULL;
	char *b_uuid = NULL;
 	const char *a_task_id = cl_get_string(a, XML_LRM_ATTR_CALLID);
 	const char *b_task_id = cl_get_string(b, XML_LRM_ATTR_CALLID);

	const char *a_key = cl_get_string(a, XML_ATTR_TRANSITION_MAGIC);
 	const char *b_key = cl_get_string(b, XML_ATTR_TRANSITION_MAGIC);

	const char *a_xml_id = ID(a);
	const char *b_xml_id = ID(b);
	
	int a_id = -1;
	int b_id = -1;

	int a_rc = -1;
	int b_rc = -1;

	int a_status = -1;
	int b_status = -1;
	
	int a_call_id = -1;
	int b_call_id = -1;

	if(safe_str_eq(a_xml_id, b_xml_id)) {
		/* We have duplicate lrm_rsc_op entries in the status
		 *    section which is unliklely to be a good thing
		 *    - we can handle it easily enough, but we need to get
		 *    to the bottom of why its happening.
		 */
		pe_err("Duplicate lrm_rsc_op entries named %s", a_xml_id);
		sort_return(0);
	}
	
	CRM_CHECK(a_task_id != NULL && b_task_id != NULL, sort_return(0));	
	a_call_id = crm_parse_int(a_task_id, NULL);
	b_call_id = crm_parse_int(b_task_id, NULL);
	
	if(a_call_id == -1 && b_call_id == -1) {
		/* both are pending ops so it doesnt matter since
		 *   stops are never pending
		 */
		sort_return(0);

	} else if(a_call_id >= 0 && a_call_id < b_call_id) {
		crm_debug_4("%s (%d) < %s (%d) : call id",
			    ID(a), a_call_id, ID(b), b_call_id);
		sort_return(-1);

	} else if(b_call_id >= 0 && a_call_id > b_call_id) {
		crm_debug_4("%s (%d) > %s (%d) : call id",
			    ID(a), a_call_id, ID(b), b_call_id);
		sort_return(1);
	}

	crm_debug_5("%s (%d) == %s (%d) : continuing",
		    ID(a), a_call_id, ID(b), b_call_id);
	
	/* now process pending ops */
	CRM_CHECK(a_key != NULL && b_key != NULL, sort_return(0));
	CRM_CHECK(decode_transition_magic(
			       a_key,&a_uuid,&a_id,&a_status, &a_rc), sort_return(0));
	CRM_CHECK(decode_transition_magic(
			       b_key,&b_uuid,&b_id,&b_status, &b_rc), sort_return(0));

	/* try and determin the relative age of the operation...
	 * some pending operations (ie. a start) may have been supuerceeded
	 *   by a subsequent stop
	 *
	 * [a|b]_id == -1 means its a shutdown operation and _always_ comes last
	 */
	if(safe_str_neq(a_uuid, b_uuid) || a_id == b_id) {
		/*
		 * some of the logic in here may be redundant...
		 *
		 * if the UUID from the TE doesnt match then one better
		 *   be a pending operation.
		 * pending operations dont survive between elections and joins
		 *   because we query the LRM directly
		 */
		
		CRM_CHECK(a_call_id == -1 || b_call_id == -1, sort_return(0));
		CRM_CHECK(a_call_id >= 0  || b_call_id >= 0, sort_return(0));

		if(b_call_id == -1) {
			crm_debug_2("%s (%d) < %s (%d) : transition + call id",
				    ID(a), a_call_id, ID(b), b_call_id);
			sort_return(-1);
		}

		if(a_call_id == -1) {
			crm_debug_2("%s (%d) > %s (%d) : transition + call id",
				    ID(a), a_call_id, ID(b), b_call_id);
			sort_return(1);
		}
		
	} else if((a_id >= 0 && a_id < b_id) || b_id == -1) {
		crm_debug_3("%s (%d) < %s (%d) : transition",
			    ID(a), a_id, ID(b), b_id);
		sort_return(-1);

	} else if((b_id >= 0 && a_id > b_id) || a_id == -1) {
		crm_debug_3("%s (%d) > %s (%d) : transition",
			    ID(a), a_id, ID(b), b_id);
		sort_return(1);
	}

	/* we should never end up here */
	crm_err("%s (%d:%d:%s) ?? %s (%d:%d:%s) : default",
		ID(a), a_call_id, a_id, a_uuid, ID(b), b_call_id, b_id, b_uuid);
	CRM_CHECK(FALSE, sort_return(0)); 
}

static gboolean
check_action_definition(resource_t *rsc, node_t *active_node, crm_data_t *xml_op,
			pe_working_set_t *data_set)
{
	char *key = NULL;
	int interval = 0;
	const char *interval_s = NULL;
	
	gboolean did_change = FALSE;

	crm_data_t *pnow = NULL;
	GHashTable *local_rsc_params = NULL;
	
	char *pnow_digest = NULL;
	const char *param_digest = NULL;
	char *local_param_digest = NULL;

#if CRM_DEPRECATED_SINCE_2_0_4
	crm_data_t *params = NULL;
#endif

	action_t *action = NULL;
	const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
	const char *op_version = crm_element_value(xml_op, XML_ATTR_CRM_VERSION);

	CRM_CHECK(active_node != NULL, return FALSE);

	interval_s = get_interval(xml_op);
	interval = crm_parse_int(interval_s, "0");
	key = generate_op_key(rsc->id, task, interval);

	if(interval > 0) {
		crm_data_t *op_match = NULL;

		crm_debug_2("Checking parameters for %s %s", key, task);
		op_match = find_rsc_op_entry(rsc, key);

		if(op_match == NULL && data_set->stop_action_orphans) {
			/* create a cancel action */
			action_t *cancel = NULL;
			crm_info("Orphan action will be stopped: %s on %s",
				 key, active_node->details->uname);

			crm_free(key);
			key = generate_op_key(rsc->id, CRMD_ACTION_CANCEL, interval);

			cancel = custom_action(
				rsc, key, CRMD_ACTION_CANCEL, active_node,
				FALSE, TRUE, data_set);

			add_hash_param(cancel->meta, XML_LRM_ATTR_TASK, task);
			add_hash_param(cancel->meta,
				       XML_LRM_ATTR_INTERVAL, interval_s);

			custom_action_order(
				rsc, NULL, cancel,
				rsc, stop_key(rsc), NULL,
				pe_ordering_optional, data_set);
		}
		if(op_match == NULL) {
			crm_debug("Orphan action detected: %s on %s",
				  key, active_node->details->uname);
			return TRUE;
		}
	}

	action = custom_action(rsc, key, task, active_node, TRUE, FALSE, data_set);
	
	local_rsc_params = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_hash_destroy_str, g_hash_destroy_str);
	
	unpack_instance_attributes(
		rsc->xml, XML_TAG_ATTR_SETS, active_node->details->attrs,
		local_rsc_params, NULL, data_set->now);
	
	pnow = create_xml_node(NULL, XML_TAG_PARAMS);
	g_hash_table_foreach(action->extra, hash2field, pnow);
	g_hash_table_foreach(rsc->parameters, hash2field, pnow);
	g_hash_table_foreach(local_rsc_params, hash2field, pnow);

	filter_action_parameters(pnow, op_version);
	pnow_digest = calculate_xml_digest(pnow, TRUE);
	param_digest = crm_element_value(xml_op, XML_LRM_ATTR_OP_DIGEST);

#if CRM_DEPRECATED_SINCE_2_0_4
	if(param_digest == NULL) {
		params = find_xml_node(xml_op, XML_TAG_PARAMS, TRUE);
	}
	if(params != NULL) {
		crm_data_t *local_params = copy_xml(params);

		crm_warn("Faking parameter digest creation for %s", ID(xml_op));		
		filter_action_parameters(local_params, op_version);
		xml_remove_prop(local_params, "interval");
		xml_remove_prop(local_params, "timeout");
		crm_log_xml_warn(local_params, "params:used");

		local_param_digest = calculate_xml_digest(local_params, TRUE);
		param_digest = local_param_digest;
		
		free_xml(local_params);
	}
#endif

	if(safe_str_neq(pnow_digest, param_digest)) {
#if CRM_DEPRECATED_SINCE_2_0_4
		if(params) {
			crm_data_t *local_params = copy_xml(params);
			filter_action_parameters(local_params, op_version);
			xml_remove_prop(local_params, "interval");
			xml_remove_prop(local_params, "timeout");
			
			free_xml(local_params);
		}
#endif
		did_change = TRUE;
		crm_log_xml_info(pnow, "params:calc");
 		crm_warn("Parameters to %s on %s changed: recorded %s vs. calculated %s",
			 ID(xml_op), active_node->details->uname,
			 crm_str(param_digest), pnow_digest);

		key = generate_op_key(rsc->id, task, interval);
		custom_action(rsc, key, task, NULL, FALSE, TRUE, data_set);
	}

	free_xml(pnow);
	crm_free(pnow_digest);
	crm_free(local_param_digest);
	g_hash_table_destroy(local_rsc_params);

	pe_free_action(action);
	
	return did_change;
}

gboolean
unpack_rsc_op(resource_t *rsc, node_t *node, crm_data_t *xml_op,
	      int *max_call_id, enum action_fail_response *on_fail,
	      pe_working_set_t *data_set) 
{
	const char *id          = NULL;
	const char *task        = NULL;
 	const char *task_id     = NULL;
 	const char *actual_rc   = NULL;
/* 	const char *target_rc   = NULL;	 */
	const char *task_status = NULL;
	const char *interval_s  = NULL;
	const char *op_digest   = NULL;

	int interval = 0;
	int task_id_i = -1;
	int task_status_i = -2;
	int actual_rc_i = 0;
	
	action_t *action = NULL;
	gboolean is_probe = FALSE;
	gboolean is_stop_action = FALSE;
	
	CRM_CHECK(rsc    != NULL, return FALSE);
	CRM_CHECK(node   != NULL, return FALSE);
	CRM_CHECK(xml_op != NULL, return FALSE);

	id	    = ID(xml_op);
	task        = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
 	task_id     = crm_element_value(xml_op, XML_LRM_ATTR_CALLID);
	task_status = crm_element_value(xml_op, XML_LRM_ATTR_OPSTATUS);
	op_digest   = crm_element_value(xml_op, XML_LRM_ATTR_OP_DIGEST);

	CRM_CHECK(id != NULL, return FALSE);
	CRM_CHECK(task != NULL, return FALSE);
	CRM_CHECK(task_status != NULL, return FALSE);

	task_status_i = crm_parse_int(task_status, NULL);

	CRM_CHECK(task_status_i <= LRM_OP_ERROR, return FALSE);
	CRM_CHECK(task_status_i >= LRM_OP_PENDING, return FALSE);

	if(safe_str_eq(task, CRMD_ACTION_NOTIFY)) {
		/* safe to ignore these */
		return TRUE;
	}

	crm_debug_2("Unpacking task %s/%s (call_id=%s, status=%s) on %s (role=%s)",
		    id, task, task_id, task_status, node->details->uname,
		    role2text(rsc->role));

	interval_s = get_interval(xml_op);
	interval = crm_parse_int(interval_s, "0");
	
	if(interval == 0 && safe_str_eq(task, CRMD_ACTION_STATUS)) {
		is_probe = TRUE;

	} else if(interval > 0 && rsc->role < RSC_ROLE_STARTED) {
		crm_debug_2("Skipping recurring action %s for stopped resource", id);
		return FALSE;
	}
	
	if(rsc->orphan) {
		crm_debug_2("Skipping param check for orphan: %s %s",
			    rsc->id, task);

	} else if(safe_str_eq(task, CRMD_ACTION_STOP)) {
		crm_debug_2("Ignoring stop params: %s", id);

	} else if(is_probe || safe_str_eq(task, CRMD_ACTION_START) || interval > 0) {
		crm_debug_3("Checking resource definition: %s", rsc->id);
		check_action_definition(rsc, node, xml_op, data_set);
	}
	
	if(safe_str_eq(task, CRMD_ACTION_STOP)) {
		is_stop_action = TRUE;
	}
	
	if(task_status_i != LRM_OP_PENDING) {
		task_id_i = crm_parse_int(task_id, "-1");

		CRM_CHECK(task_id != NULL, return FALSE);
		CRM_CHECK(task_id_i >= 0, return FALSE);
		CRM_CHECK(task_id_i > *max_call_id, return FALSE);
	}

	if(*max_call_id < task_id_i) {
		*max_call_id = task_id_i;
	}
	
	if(node->details->unclean) {
		crm_debug_2("Node %s (where %s is running) is unclean."
			  " Further action depends on the value of %s",
			  node->details->uname, rsc->id, XML_RSC_ATTR_STOPFAIL);
	}

	actual_rc = crm_element_value(xml_op, XML_LRM_ATTR_RC);
	CRM_CHECK(actual_rc != NULL, return FALSE);	
	actual_rc_i = crm_parse_int(actual_rc, NULL);
	
	if(EXECRA_NOT_RUNNING == actual_rc_i) {
		if(is_probe) {
			/* treat these like stops */
			is_stop_action = TRUE;
		}
		if(is_stop_action) {
			task_status_i = LRM_OP_DONE;
 		} else {
			CRM_CHECK(task_status_i == LRM_OP_ERROR,
				task_status_i = LRM_OP_ERROR);
		}
		
	} else if(EXECRA_RUNNING_MASTER == actual_rc_i) {
		if(is_probe
		   || (rsc->role == RSC_ROLE_MASTER
		       && safe_str_eq(task, CRMD_ACTION_STATUS))) {
			task_status_i = LRM_OP_DONE;
		} else {
			if(rsc->role != RSC_ROLE_MASTER) {
				crm_err("%s reported %s in master mode on %s",
					id, rsc->id,
					node->details->uname);
			}
			
			CRM_CHECK(task_status_i == LRM_OP_ERROR,
				task_status_i = LRM_OP_ERROR);
		}
		rsc->role = RSC_ROLE_MASTER;

	} else if(EXECRA_FAILED_MASTER == actual_rc_i) {
		rsc->role = RSC_ROLE_MASTER;
		task_status_i = LRM_OP_ERROR;

	} else if(EXECRA_OK == actual_rc_i
		  && interval > 0
		  && rsc->role == RSC_ROLE_MASTER) {
		/* catch status ops that return 0 instead of 8 while they
		 *   are supposed to be in master mode
		 */
		task_status_i = LRM_OP_ERROR;
	}

	if(task_status_i == LRM_OP_ERROR
	   || task_status_i == LRM_OP_TIMEOUT
	   || task_status_i == LRM_OP_NOTSUPPORTED) {
		action = custom_action(rsc, crm_strdup(id), task, NULL,
				       TRUE, FALSE, data_set);
		if(action->on_fail == action_fail_ignore) {
			task_status_i = LRM_OP_DONE;
		}
	}
	
	switch(task_status_i) {
		case LRM_OP_PENDING:
			if(safe_str_eq(task, CRMD_ACTION_START)) {
				rsc->start_pending = TRUE;
				rsc->role = RSC_ROLE_STARTED;
				
			} else if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				rsc->role = RSC_ROLE_MASTER;
			}
			break;
		
		case LRM_OP_DONE:
			crm_debug_3("%s/%s completed on %s",
				    rsc->id, task, node->details->uname);

			if(is_stop_action) {
				rsc->role = RSC_ROLE_STOPPED;

				/* clear any previous failure actions */
				*on_fail = action_fail_ignore;
				rsc->next_role = RSC_ROLE_UNKNOWN;
				
			} else if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				rsc->role = RSC_ROLE_MASTER;

			} else if(safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
				rsc->role = RSC_ROLE_SLAVE;
				
			} else if(rsc->role < RSC_ROLE_STARTED) {
				crm_debug_3("%s active on %s",
					    rsc->id, node->details->uname);
				rsc->role = RSC_ROLE_STARTED;
			}
			break;

		case LRM_OP_ERROR:
		case LRM_OP_TIMEOUT:
		case LRM_OP_NOTSUPPORTED:
			crm_warn("Processing failed op (%s) for %s on %s",
				 id, rsc->id, node->details->uname);

			if(*on_fail < action->on_fail) {
				*on_fail = action->on_fail;
			}
			
			if(task_status_i == LRM_OP_NOTSUPPORTED
			   || is_stop_action
			   || safe_str_eq(task, CRMD_ACTION_START) ) {
				crm_warn("Handling failed %s for %s on %s",
					 task, rsc->id, node->details->uname);
				rsc2node_new("dont_run__failed_stopstart",
					     rsc, -INFINITY, node, data_set);
			}
			
			if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				rsc->role = RSC_ROLE_MASTER;

			} else if(safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
				rsc->role = RSC_ROLE_MASTER;
				
			} else if(rsc->role < RSC_ROLE_STARTED) {
				rsc->role = RSC_ROLE_STARTED;
			}

			crm_debug_2("Resource %s: role=%s, unclean=%s, on_fail=%s, fail_role=%s",
				    rsc->id, role2text(rsc->role),
				    node->details->unclean?"true":"false",
				    fail2text(action->on_fail),
				    role2text(action->fail_role));

			if(action->fail_role != RSC_ROLE_STARTED
			   && rsc->next_role < action->fail_role) {
				rsc->next_role = action->fail_role;
			}

			if(action->fail_role == RSC_ROLE_STOPPED) {
				/* make sure it doesnt come up again */
				pe_free_shallow_adv(rsc->allowed_nodes, TRUE);
				rsc->allowed_nodes = node_list_dup(
					data_set->nodes, FALSE, FALSE);
				slist_iter(
					node, node_t, rsc->allowed_nodes, lpc,
					node->weight = -INFINITY;
					);
			}
			
			pe_free_action(action);
			action = NULL;
			break;
		case LRM_OP_CANCELLED:
			/* do nothing?? */
			pe_err("Dont know what to do for cancelled ops yet");
			break;
	}

	crm_debug_3("Resource %s after %s: role=%s",
		    rsc->id, task, role2text(rsc->role));

	pe_free_action(action);
	
	return TRUE;
}

gboolean
rsc_colocation_new(const char *id, enum con_strength strength,
		   resource_t *rsc_lh, resource_t *rsc_rh,
		   const char *state_lh, const char *state_rh)
{
	rsc_colocation_t *new_con      = NULL;
 	rsc_colocation_t *inverted_con = NULL; 

	if(rsc_lh == NULL){
		pe_config_err("No resource found for LHS %s", id);
		return FALSE;

	} else if(rsc_rh == NULL){
		pe_config_err("No resource found for RHS of %s", id);
		return FALSE;
	}

	crm_malloc0(new_con, sizeof(rsc_colocation_t));
	if(new_con == NULL) {
		return FALSE;
	}
	if(safe_str_eq(state_lh, CRMD_ACTION_STARTED)) {
		state_lh = NULL;
	}
	if(safe_str_eq(state_rh, CRMD_ACTION_STARTED)) {
		state_rh = NULL;
	}

	new_con->id       = id;
	new_con->rsc_lh   = rsc_lh;
	new_con->rsc_rh   = rsc_rh;
	new_con->strength = strength;
	new_con->state_lh = state_lh;
	new_con->state_rh = state_rh;

	inverted_con = invert_constraint(new_con);
	
	crm_debug_4("Adding constraint %s (%p) to %s",
		  new_con->id, new_con, rsc_lh->id);
	
	rsc_lh->rsc_cons = g_list_insert_sorted(
		rsc_lh->rsc_cons, new_con, sort_cons_strength);
	
	crm_debug_4("Adding constraint %s (%p) to %s",
		  inverted_con->id, inverted_con, rsc_rh->id);
	
	rsc_rh->rsc_cons = g_list_insert_sorted(
		rsc_rh->rsc_cons, inverted_con, sort_cons_strength);
	
	return TRUE;
}

/* LHS before RHS */
gboolean
custom_action_order(
	resource_t *lh_rsc, char *lh_action_task, action_t *lh_action,
	resource_t *rh_rsc, char *rh_action_task, action_t *rh_action,
	enum pe_ordering type, pe_working_set_t *data_set)
{
	order_constraint_t *order = NULL;

	if((lh_action == NULL && lh_rsc == NULL)
	   || (rh_action == NULL && rh_rsc == NULL)){
		pe_config_err("Invalid inputs lh_rsc=%p, lh_a=%p,"
			      " rh_rsc=%p, rh_a=%p",
			      lh_rsc, lh_action, rh_rsc, rh_action);
		crm_free(lh_action_task);
		crm_free(rh_action_task);
		return FALSE;
	}

	crm_malloc0(order, sizeof(order_constraint_t));
	if(order == NULL) { return FALSE; }
	
	order->id             = data_set->order_id++;
	order->type           = type;
	order->lh_rsc         = lh_rsc;
	order->rh_rsc         = rh_rsc;
	order->lh_action      = lh_action;
	order->rh_action      = rh_action;
	order->lh_action_task = lh_action_task;
	order->rh_action_task = rh_action_task;
	
	data_set->ordering_constraints = g_list_append(
		data_set->ordering_constraints, order);
	
	if(lh_rsc != NULL && rh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " %s/%s before %s/%s",
			 order->id, ordering_type2text(order->type),
			 lh_rsc->id, lh_action_task,
			 rh_rsc->id, rh_action_task);
		
	} else if(lh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " %s/%s before action %d (%s)",
			 order->id, ordering_type2text(order->type),
			 lh_rsc->id, lh_action_task,
			 rh_action->id, rh_action_task);
		
	} else if(rh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " action %d (%s) before %s/%s",
			 order->id, ordering_type2text(order->type),
			 lh_action->id, lh_action_task,
			 rh_rsc->id, rh_action_task);
		
	} else {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " action %d (%s) before action %d (%s)",
			 order->id, ordering_type2text(order->type),
			 lh_action->id, lh_action_task,
			 rh_action->id, rh_action_task);
	}
	
	return TRUE;
}

gboolean
unpack_rsc_colocation(crm_data_t * xml_obj, pe_working_set_t *data_set)
{
	enum con_strength strength_e = pecs_ignore;

	const char *id    = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *id_rh = crm_element_value(xml_obj, XML_CONS_ATTR_TO);
	const char *id_lh = crm_element_value(xml_obj, XML_CONS_ATTR_FROM);
	const char *score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
	const char *state_lh = crm_element_value(xml_obj, XML_RULE_ATTR_FROMSTATE);
	const char *state_rh = crm_element_value(xml_obj, XML_RULE_ATTR_TOSTATE);

	resource_t *rsc_lh = pe_find_resource(data_set->resources, id_lh);
	resource_t *rsc_rh = pe_find_resource(data_set->resources, id_rh);
 
	if(rsc_lh == NULL) {
		pe_config_err("No resource (con=%s, rsc=%s)", id, id_lh);
		return FALSE;
		
	} else if(rsc_rh == NULL) {
		pe_config_err("No resource (con=%s, rsc=%s)", id, id_rh);
		return FALSE;
	}

	/* the docs indicate that only +/- INFINITY are allowed,
	 *   but no-one ever reads the docs so all positive values will
	 *   count as "must" and negative values as "must not"
	 */
	if(score == NULL || score[0] != '-') {
		strength_e = pecs_must;
	} else {
		strength_e = pecs_must_not;
	}
	return rsc_colocation_new(id, strength_e, rsc_lh, rsc_rh,
				  state_lh, state_rh);
}

static const char *
invert_action(const char *action) 
{
	if(safe_str_eq(action, CRMD_ACTION_START)) {
		return CRMD_ACTION_STOP;

	} else if(safe_str_eq(action, CRMD_ACTION_STOP)) {
		return CRMD_ACTION_START;
		
	} else if(safe_str_eq(action, CRMD_ACTION_PROMOTE)) {
		return CRMD_ACTION_DEMOTE;
		
	} else if(safe_str_eq(action, CRMD_ACTION_DEMOTE)) {
		return CRMD_ACTION_PROMOTE;

	} else if(safe_str_eq(action, CRMD_ACTION_STARTED)) {
		return CRMD_ACTION_STOPPED;
		
	} else if(safe_str_eq(action, CRMD_ACTION_STOPPED)) {
		return CRMD_ACTION_STARTED;
		
	}
	pe_err("Unknown action: %s", action);
	return NULL;
}


gboolean
unpack_rsc_order(crm_data_t * xml_obj, pe_working_set_t *data_set)
{
	gboolean symmetrical_bool = TRUE;
	
	const char *id     = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *type   = crm_element_value(xml_obj, XML_ATTR_TYPE);
	const char *id_rh  = crm_element_value(xml_obj, XML_CONS_ATTR_TO);
	const char *id_lh  = crm_element_value(xml_obj, XML_CONS_ATTR_FROM);
	const char *action = crm_element_value(xml_obj, XML_CONS_ATTR_ACTION);
	const char *action_rh = crm_element_value(xml_obj, XML_CONS_ATTR_TOACTION);

	const char *symmetrical = crm_element_value(
		xml_obj, XML_CONS_ATTR_SYMMETRICAL);

	resource_t *rsc_lh   = NULL;
	resource_t *rsc_rh   = NULL;

	if(xml_obj == NULL) {
		pe_config_err("No constraint object to process.");
		return FALSE;

	} else if(id == NULL) {
		pe_config_err("%s constraint must have an id",
			crm_element_name(xml_obj));
		return FALSE;
		
	} else if(id_lh == NULL || id_rh == NULL) {
		pe_config_err("Constraint %s needs two sides lh: %s rh: %s",
			      id, crm_str(id_lh), crm_str(id_rh));
		return FALSE;
	}

	if(action == NULL) {
		action = CRMD_ACTION_START;
	}
	if(action_rh == NULL) {
		action_rh = action;
	}
	CRM_CHECK(action != NULL, return FALSE);
	CRM_CHECK(action_rh != NULL, return FALSE);
	
	if(safe_str_eq(type, "before")) {
		id_lh  = crm_element_value(xml_obj, XML_CONS_ATTR_TO);
		id_rh  = crm_element_value(xml_obj, XML_CONS_ATTR_FROM);
		action = crm_element_value(xml_obj, XML_CONS_ATTR_TOACTION);
		action_rh = crm_element_value(xml_obj, XML_CONS_ATTR_ACTION);
		if(action_rh == NULL) {
			action_rh = CRMD_ACTION_START;
		}
		if(action == NULL) {
			action = action_rh;
		}
	}

	CRM_CHECK(action != NULL, return FALSE);
	CRM_CHECK(action_rh != NULL, return FALSE);
	
	rsc_lh   = pe_find_resource(data_set->resources, id_rh);
	rsc_rh   = pe_find_resource(data_set->resources, id_lh);

	if(rsc_lh == NULL) {
		pe_config_err("Constraint %s: no resource found for LHS of %s", id, id_lh);
		return FALSE;
	
	} else if(rsc_rh == NULL) {
		pe_config_err("Constraint %s: no resource found for RHS of %s", id, id_rh);
		return FALSE;
	}

	custom_action_order(
		rsc_lh, generate_op_key(rsc_lh->id, action, 0), NULL,
		rsc_rh, generate_op_key(rsc_rh->id, action_rh, 0), NULL,
		pe_ordering_optional, data_set);

	if(rsc_rh->restart_type == pe_restart_restart
	   && safe_str_eq(action, action_rh)) {
		if(safe_str_eq(action, CRMD_ACTION_START)) {
			crm_debug_2("Recover start-start: %s-%s",
				rsc_lh->id, rsc_rh->id);
  			order_start_start(rsc_lh, rsc_rh, pe_ordering_recover);
 		} else if(safe_str_eq(action, CRMD_ACTION_STOP)) {
			crm_debug_2("Recover stop-stop: %s-%s",
				rsc_rh->id, rsc_lh->id);
  			order_stop_stop(rsc_rh, rsc_lh, pe_ordering_recover); 
		}
	}

	cl_str_to_boolean(symmetrical, &symmetrical_bool);
	if(symmetrical_bool == FALSE) {
		return TRUE;
	}
	
	action = invert_action(action);
	action_rh = invert_action(action_rh);
	
	custom_action_order(
		rsc_rh, generate_op_key(rsc_rh->id, action_rh, 0), NULL,
		rsc_lh, generate_op_key(rsc_lh->id, action, 0), NULL,
		pe_ordering_optional, data_set);

	if(rsc_lh->restart_type == pe_restart_restart
	   && safe_str_eq(action, action_rh)) {
		if(safe_str_eq(action, CRMD_ACTION_START)) {
			crm_debug_2("Recover start-start (2): %s-%s",
				rsc_lh->id, rsc_rh->id);
  			order_start_start(rsc_lh, rsc_rh, pe_ordering_recover);
		} else if(safe_str_eq(action, CRMD_ACTION_STOP)) { 
			crm_debug_2("Recover stop-stop (2): %s-%s",
				rsc_rh->id, rsc_lh->id);
  			order_stop_stop(rsc_rh, rsc_lh, pe_ordering_recover); 
		}
	}
	
	return TRUE;
}

gboolean
add_node_attrs(crm_data_t *xml_obj, node_t *node, pe_working_set_t *data_set)
{
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("#"XML_ATTR_UNAME),
			    crm_strdup(node->details->uname));
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("#"XML_ATTR_ID),
			    crm_strdup(node->details->id));
	if(safe_str_eq(node->details->id, data_set->dc_uuid)) {
		data_set->dc_node = node;
		node->details->is_dc = TRUE;
		g_hash_table_insert(node->details->attrs,
				    crm_strdup("#"XML_ATTR_DC),
				    crm_strdup(XML_BOOLEAN_TRUE));
	} else {
		g_hash_table_insert(node->details->attrs,
				    crm_strdup("#"XML_ATTR_DC),
				    crm_strdup(XML_BOOLEAN_FALSE));
	}
	
	unpack_instance_attributes(
		xml_obj, XML_TAG_ATTR_SETS, NULL,
		node->details->attrs, NULL, data_set->now);

	return TRUE;
}

gboolean
unpack_rsc_location(crm_data_t * xml_obj, pe_working_set_t *data_set)
{
	const char *id_lh   = crm_element_value(xml_obj, "rsc");
	const char *id      = crm_element_value(xml_obj, XML_ATTR_ID);
	resource_t *rsc_lh  = pe_find_resource(data_set->resources, id_lh);
	
	if(rsc_lh == NULL) {
		/* only a warn as BSC adds the constraint then the resource */
		pe_config_warn("No resource (con=%s, rsc=%s)", id, id_lh);
		return FALSE;

	} else if(rsc_lh->is_managed == FALSE) {
		crm_debug_2("Ignoring constraint %s: resource %s not managed",
			    id, id_lh);
		return FALSE;
	}

	xml_child_iter_filter(
		xml_obj, rule_xml, XML_TAG_RULE,
		crm_debug_2("Unpacking %s/%s", id, ID(rule_xml));
		generate_location_rule(rsc_lh, rule_xml, data_set);
		);
	return TRUE;
}

rsc_to_node_t *
generate_location_rule(
	resource_t *rsc, crm_data_t *rule_xml, pe_working_set_t *data_set)
{	
	const char *rule_id = NULL;
	const char *score   = NULL;
	const char *boolean = NULL;
	const char *role    = NULL;
	const char *attr_score = NULL;

	GListPtr match_L  = NULL;
	
	int score_f   = 0;
	gboolean do_and = TRUE;
	gboolean accept = TRUE;
	gboolean raw_score = TRUE;
	
	rsc_to_node_t *location_rule = NULL;
	
	rule_id = crm_element_value(rule_xml, XML_ATTR_ID);
	boolean = crm_element_value(rule_xml, XML_RULE_ATTR_BOOLEAN_OP);
	role = crm_element_value(rule_xml, XML_RULE_ATTR_ROLE);

	crm_debug_2("Processing rule: %s", rule_id);

	if(role != NULL && text2role(role) == RSC_ROLE_UNKNOWN) {
		pe_err("Bad role specified for %s: %s", rule_id, role);
		return NULL;
	}
	
	score = crm_element_value(rule_xml, XML_RULE_ATTR_SCORE);
	if(score != NULL) {
		score_f = char2score(score);

	} else {
		score = crm_element_value(
			rule_xml, XML_RULE_ATTR_SCORE_ATTRIBUTE);
		if(score == NULL) {
			score = crm_element_value(
				rule_xml, XML_RULE_ATTR_SCORE_MANGLED);
		}
		if(score != NULL) {
			raw_score = FALSE;
		}
	}
	
	if(safe_str_eq(boolean, "or")) {
		do_and = FALSE;
	}
	
	location_rule = rsc2node_new(rule_id, rsc, 0, NULL, data_set);
	
	if(location_rule == NULL) {
		return NULL;
	}
	if(role != NULL) {
		crm_debug_2("Setting role filter: %s", role);
		location_rule->role_filter = text2role(role);
	}
	if(do_and) {
		match_L = node_list_dup(data_set->nodes, TRUE, FALSE);
		slist_iter(
			node, node_t, match_L, lpc,
			node->weight = score_f;
			);
	}

	xml_child_iter(
		rule_xml, expr, 		

		enum expression_type type = find_expression_type(expr);
		if(type == not_expr) {
			pe_err("Expression <%s id=%s...> is not valid",
			       crm_element_name(expr), crm_str(ID(expr)));
			continue;	
		}	
		
		slist_iter(
			node, node_t, data_set->nodes, lpc,

			if(type == nested_rule) {
				accept = test_rule(
					expr, node->details->attrs,
					RSC_ROLE_UNKNOWN, data_set->now);
			} else {
				accept = test_expression(
					expr, node->details->attrs,
					RSC_ROLE_UNKNOWN, data_set->now);
			}
			
			if(raw_score == FALSE) {
				attr_score = g_hash_table_lookup(
					node->details->attrs, score);
				if(attr_score == NULL) {
					accept = FALSE;
					pe_warn("node %s did not have a value"
						" for %s",
						node->details->uname, score);
				} else {
					score_f = char2score(score);
				}
			}
			
			if(!do_and && accept) {
				node_t *local = pe_find_node_id(
					match_L, node->details->id);
				if(local == NULL) {
					local = node_copy(node);
					match_L = g_list_append(match_L, local);
				}
				local->weight = merge_weights(
					local->weight, score_f);
				crm_debug_5("node %s already matched",
					    node->details->uname);
				
			} else if(do_and && !accept) {
				/* remove it */
				node_t *delete = pe_find_node_id(
					match_L, node->details->id);
				if(delete != NULL) {
					match_L = g_list_remove(match_L,delete);
					crm_debug_5("node %s did not match",
						    node->details->uname);
				}
				crm_free(delete);
			}
			);
		);
	
	location_rule->node_list_rh = match_L;
	if(location_rule->node_list_rh == NULL) {
		crm_debug_2("No matching nodes for rule %s", rule_id);
		return NULL;
	} 

	crm_debug_3("%s: %d nodes matched",
		    rule_id, g_list_length(location_rule->node_list_rh));
	crm_action_debug_3(print_rsc_to_node("Added", location_rule, FALSE));
	return location_rule;
}