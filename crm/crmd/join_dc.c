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

#include <heartbeat.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <crmd_fsa.h>
#include <crmd_messages.h>

#include <crm/dmalloc_wrapper.h>

GHashTable *welcomed_nodes   = NULL;
GHashTable *integrated_nodes = NULL;
GHashTable *finalized_nodes  = NULL;
GHashTable *confirmed_nodes  = NULL;
char *max_epoche = NULL;
char *max_generation_from = NULL;
crm_data_t *max_generation_xml = NULL;

void initialize_join(gboolean before);
gboolean finalize_join_for(gpointer key, gpointer value, gpointer user_data);
void join_send_offer(gpointer key, gpointer value, gpointer user_data);
void finalize_sync_callback(const HA_Message *msg, int call_id, int rc,
			    crm_data_t *output, void *user_data);

/*	 A_DC_JOIN_OFFER_ALL	*/
enum crmd_fsa_input
do_dc_join_offer_all(long long action,
		     enum crmd_fsa_cause cause,
		     enum crmd_fsa_state cur_state,
		     enum crmd_fsa_input current_input,
		     fsa_data_t *msg_data)
{
	/* reset everyones status back to down or in_ccm in the CIB */
#if 0
	crm_data_t *cib_copy   = get_cib_copy(fsa_cib_conn);
	crm_data_t *tmp1       = get_object_root(XML_CIB_TAG_STATUS, cib_copy);
	crm_data_t *tmp2       = NULL;
#endif
	crm_data_t *fragment   = create_cib_fragment(NULL, NULL);
	crm_data_t *update     = find_xml_node(fragment, XML_TAG_CIB, TRUE);
	
	/* catch any nodes that are active in the CIB but not in the CCM list*/
	update = get_object_root(XML_CIB_TAG_STATUS, update);
	CRM_DEV_ASSERT(update != NULL);

#if 0
	/* dont do this for now... involves too much blocking and I cant
	 * think of a sensible way to do it asyncronously
	 */
	xml_child_iter(
		tmp1, node_entry, XML_CIB_TAG_STATE,

		const char *node_id = crm_element_value(node_entry, XML_ATTR_UNAME);
		gpointer a_node = g_hash_table_lookup(
			fsa_membership_copy->members, node_id);

		if(a_node != NULL || (safe_str_eq(fsa_our_uname, node_id))) {
			/* handled by do_update_cib_node() */
			continue;
		}

		tmp2 = create_node_state(
			node_id, node_id, NULL,
			XML_BOOLEAN_NO, NULL, CRMD_JOINSTATE_PENDING, NULL);

		add_node_copy(update, tmp2);
		free_xml(tmp2);
		);
	free_xml(cib_copy);
#endif
	
	/* now process the CCM data */
	do_update_cib_nodes(fragment, TRUE);
	
#if 0
	/* Avoid ordered message delays caused when the CRMd proc
	 * isnt running yet (ie. send as a broadcast msg which are never
	 * sent ordered.
	 */
	send_request(NULL, NULL, CRM_OP_WELCOME,
		     NULL, CRM_SYSTEM_CRMD, NULL);	
#else

	crm_debug("0) Offering membership to %d clients",
		  fsa_membership_copy->members_size);
	
	g_hash_table_foreach(
		fsa_membership_copy->members, join_send_offer, NULL);
	
#endif

	/* dont waste time by invoking the pe yet; */
	crm_debug("1) Waiting on %d outstanding join acks",
		  g_hash_table_size(welcomed_nodes));

	return I_NULL;
}

/*	 A_DC_JOIN_OFFER_ONE	*/
enum crmd_fsa_input
do_dc_join_offer_one(long long action,
		     enum crmd_fsa_cause cause,
		     enum crmd_fsa_state cur_state,
		     enum crmd_fsa_input current_input,
		     fsa_data_t *msg_data)
{
	oc_node_t member;
	gpointer a_node = NULL;
	ha_msg_input_t *welcome = fsa_typed_data(fsa_dt_ha_msg);
	const char *join_to = NULL;

	if(welcome == NULL) {
		crm_err("Attempt to send welcome message "
			"without a message to reply to!");
		return I_NULL;
	}
	
	join_to = cl_get_string(welcome->msg, F_CRM_HOST_FROM);

	g_hash_table_remove(confirmed_nodes,  join_to);
	g_hash_table_remove(finalized_nodes,  join_to);
	g_hash_table_remove(integrated_nodes, join_to);
	g_hash_table_remove(welcomed_nodes,   join_to);

	if(a_node != NULL
	   && (cur_state == S_INTEGRATION || cur_state == S_FINALIZE_JOIN)) {
		/* note: it _is_ possible that a node will have been
		 *  sick or starting up when the original offer was made.
		 *  however, it will either re-announce itself in due course
		 *  _or_ we can re-store the original offer on the client.
		 */
		crm_info("Re-offering membership to %s...", join_to);
	}

	crm_debug("Processing annouce request from %s in state %s",
		  join_to, fsa_state2string(cur_state));

	member.node_uname = crm_strdup(join_to);
	join_send_offer(NULL, &member, NULL);
	crm_free(member.node_uname);
	
	/* this was a genuine join request, cancel any existing
	 * transition and invoke the PE
	 */
	register_fsa_input_w_actions(
		msg_data->fsa_cause, I_NULL, NULL, A_TE_CANCEL);

	/* dont waste time by invoking the pe yet; */
	crm_debug("1) Waiting on %d outstanding join acks",
		  g_hash_table_size(welcomed_nodes));
	
	return I_NULL;
}

/*	 A_DC_JOIN_PROCESS_REQ	*/
enum crmd_fsa_input
do_dc_join_req(long long action,
	       enum crmd_fsa_cause cause,
	       enum crmd_fsa_state cur_state,
	       enum crmd_fsa_input current_input,
	       fsa_data_t *msg_data)
{
	crm_data_t *generation = NULL;

	gboolean is_a_member = FALSE;
	const char *ack_nack = CRMD_JOINSTATE_MEMBER;
	ha_msg_input_t *join_ack = fsa_typed_data(fsa_dt_ha_msg);

	const char *join_from = cl_get_string(join_ack->msg,F_CRM_HOST_FROM);
	const char *ref       = cl_get_string(join_ack->msg,XML_ATTR_REFERENCE);
	const char *op	   = cl_get_string(join_ack->msg, F_CRM_TASK);

	gpointer join_node =
		g_hash_table_lookup(fsa_membership_copy->members, join_from);

	if(safe_str_neq(op, CRM_OP_WELCOME)) {
		crm_warn("Ignoring op=%s message", op);
		return I_NULL;
	}

	crm_devel("Processing req from %s", join_from);
	
	if(join_node != NULL) {
		is_a_member = TRUE;
	}
	
	generation = join_ack->xml;

	if(max_generation_xml == NULL) {
		max_generation_xml = copy_xml_node_recursive(generation);
		max_generation_from = crm_strdup(join_from);

	} else if(cib_compare_generation(max_generation_xml, generation) < 0) {
		clear_bit_inplace(fsa_input_register, R_HAVE_CIB);
		crm_devel("%s has a better generation number than the current max",
			  join_from);
		crm_xml_devel(max_generation_xml, "Max generation");
		crm_xml_devel(generation, "Their generation");
		crm_free(max_generation_from);
		free_xml(max_generation_xml);
		
		max_generation_from = crm_strdup(join_from);
		max_generation_xml = copy_xml_node_recursive(join_ack->xml);
	}
	
	crm_debug("2) Welcoming node %s after ACK (ref %s)", join_from, ref);

	/* add them to our list of CRMD_STATE_ACTIVE nodes */
	
	if(/* some reason */ 0) {
		/* NACK this client */
		ack_nack = CRMD_JOINSTATE_DOWN;
	}	

	g_hash_table_insert(
		integrated_nodes, crm_strdup(join_from), crm_strdup(ack_nack));

	crm_debug("%u nodes have been integrated",
		  g_hash_table_size(integrated_nodes));
	
	g_hash_table_remove(welcomed_nodes, join_from);

	if(g_hash_table_size(welcomed_nodes) == 0) {
		crm_info("That was the last outstanding join ack");
		register_fsa_input(C_FSA_INTERNAL, I_INTEGRATED, NULL);

	} else {
		/* dont waste time by invoking the PE yet; */
		crm_debug("Still waiting on %d outstanding join acks",
			  g_hash_table_size(welcomed_nodes));
	}
	return I_NULL;
}



/*	A_DC_JOIN_FINALIZE	*/
enum crmd_fsa_input
do_dc_join_finalize(long long action,
		    enum crmd_fsa_cause cause,
		    enum crmd_fsa_state cur_state,
		    enum crmd_fsa_input current_input,
		    fsa_data_t *msg_data)
{
	enum cib_errors rc = cib_ok;

	/* This we can do straight away and avoid clients timing us out
	 *  while we compute the latest CIB
	 */
	crm_debug("Notifying %d clients of join results",
		  g_hash_table_size(integrated_nodes));
	g_hash_table_foreach_remove(
		integrated_nodes, finalize_join_for, GINT_TO_POINTER(TRUE));
	
	if(max_generation_from == NULL
	   || safe_str_eq(max_generation_from, fsa_our_uname)){
		set_bit_inplace(fsa_input_register, R_HAVE_CIB);
	}
	
	if(is_set(fsa_input_register, R_HAVE_CIB) == FALSE) {
		/* ask for the agreed best CIB */
		crm_info("Asking %s for its copy of the CIB",
			 crm_str(max_generation_from));

		fsa_cib_conn->call_timeout = 10;
		rc = fsa_cib_conn->cmds->sync_from(
			fsa_cib_conn, max_generation_from, NULL, cib_quorum_override);
		fsa_cib_conn->call_timeout = 0; /* back to the default */
		add_cib_op_callback(rc, FALSE, crm_strdup(max_generation_from),
				    finalize_sync_callback);
		return I_NULL;
	}

	crm_devel("Bumping the epoche and syncing to %d clients",
		  g_hash_table_size(finalized_nodes));
	fsa_cib_conn->cmds->bump_epoch(
		fsa_cib_conn, cib_scope_local|cib_quorum_override);
	rc = fsa_cib_conn->cmds->sync(fsa_cib_conn, NULL, cib_quorum_override);
	add_cib_op_callback(rc, FALSE, NULL, finalize_sync_callback);

	return I_NULL;
}

void
finalize_sync_callback(const HA_Message *msg, int call_id, int rc,
		       crm_data_t *output, void *user_data) 
{
	CRM_DEV_ASSERT(cib_not_master != rc);
	if(rc == cib_remote_timeout) {
		crm_err("Sync from %s resulted in an error: %s."
			"  Use what we have...",
			(char*)user_data, cib_error2string(rc));
#if 0
		/* restart the whole join process */
		register_fsa_error_adv(C_FSA_INTERNAL, I_ELECTION_DC,
				       NULL, NULL, __FUNCTION__);
#else
		rc = cib_ok;
#endif
	} else if(rc < cib_ok) {
		crm_err("Sync from %s resulted in an error: %s",
			(char*)user_data, cib_error2string(rc));
		
		register_fsa_error_adv(
			C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
	} else {
		fsa_cib_conn->cmds->bump_epoch(
			fsa_cib_conn, cib_quorum_override);
	}
	crm_free(user_data);
}

/*	A_DC_JOIN_PROCESS_ACK	*/
enum crmd_fsa_input
do_dc_join_ack(long long action,
	       enum crmd_fsa_cause cause,
	       enum crmd_fsa_state cur_state,
	       enum crmd_fsa_input current_input,
	       fsa_data_t *msg_data)
{
	/* now update them to "member" */
	crm_data_t *update = NULL;
	ha_msg_input_t *join_ack = fsa_typed_data(fsa_dt_ha_msg);
	const char *join_from = cl_get_string(join_ack->msg, F_CRM_HOST_FROM);
	const char *op = cl_get_string(join_ack->msg, F_CRM_TASK);
	const char *type = cl_get_string(join_ack->msg, F_SUBTYPE);
	const char *join_state = NULL;


	if(safe_str_neq(op, CRM_OP_JOINACK)) {
		crm_warn("Ignoring op=%s message", op);
		return I_NULL;

	} else if(safe_str_eq(type, XML_ATTR_REQUEST)) {
		crm_verbose("Ignoring request");
		crm_log_message(LOG_VERBOSE, join_ack->msg);
		return I_NULL;
	}
	
	crm_debug("Processing ack from %s", join_from);

	join_state = (const char *)
		g_hash_table_lookup(finalized_nodes, join_from);
	
	if(join_state == NULL) {
		crm_err("Join not in progress: ignoring join from %s",
			join_from);
		register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
		return I_NULL;
		
	} else if(safe_str_neq(join_state, CRMD_JOINSTATE_MEMBER)) {
		crm_err("Node %s wasnt invited to join the cluster",join_from);
		g_hash_table_remove(finalized_nodes, join_from);
		return I_NULL;
	} else {
		g_hash_table_remove(finalized_nodes, join_from);
	}
	
	if(g_hash_table_lookup(confirmed_nodes, join_from) != NULL) {
		crm_err("hash already contains confirmation from %s", join_from);
	}
	
	g_hash_table_insert(confirmed_nodes, crm_strdup(join_from),
			    crm_strdup(CRMD_JOINSTATE_MEMBER));

	/* the updates will actually occur in reverse order because of
	 * the LIFO nature of the fsa input queue
	 */
	
	/* update CIB with the current LRM status from the node */
	update_local_cib(copy_xml_node_recursive(join_ack->xml));

	/* update node entry in the status section  */
	crm_info("4) Updating node state to %s for %s", join_state, join_from);
	update = create_node_state(
		join_from, join_from,
		ACTIVESTATUS, NULL, ONLINESTATUS, join_state, join_state);

	set_xml_property_copy(update,XML_CIB_ATTR_EXPSTATE, CRMD_STATE_ACTIVE);

	update_local_cib(create_cib_fragment(update, NULL));

	if(g_hash_table_size(finalized_nodes) == 0) {
		crm_info("That was the last outstanding join confirmation");
		register_fsa_input_later(C_FSA_INTERNAL, I_FINALIZED, NULL);
		
		return I_NULL;
	}

	/* dont waste time by invoking the pe yet; */
	crm_debug("Still waiting on %d outstanding join confirmations",
		  g_hash_table_size(finalized_nodes));
	
	return I_NULL;
}

gboolean
finalize_join_for(gpointer key, gpointer value, gpointer user_data)
{
	const char *join_to = NULL;
	const char *join_state = NULL;
	HA_Message *acknak = NULL;
	
	if(key == NULL || value == NULL) {
		return (gboolean)GPOINTER_TO_INT(user_data);
	}

	join_to    = (const char *)key;
	join_state = (const char *)value;

	/* make sure the node exists in the config section */
	create_node_entry(join_to, join_to, CRMD_JOINSTATE_MEMBER);

	/* send the ack/nack to the node */
	acknak = create_request(
		CRM_OP_JOINACK, NULL, join_to,
		CRM_SYSTEM_CRMD, CRM_SYSTEM_DC, NULL);
	
	/* set the ack/nack */
	if(safe_str_eq(join_state, CRMD_JOINSTATE_MEMBER)) {
		crm_info("3) ACK'ing join request from %s, state %s",
			 join_to, join_state);
		ha_msg_add(acknak, CRM_OP_JOINACK, XML_BOOLEAN_TRUE);
		g_hash_table_insert(
			finalized_nodes,
			crm_strdup(join_to), crm_strdup(CRMD_JOINSTATE_MEMBER));

	} else {
		crm_warn("3) NACK'ing join request from %s, state %s",
			 join_to, join_state);
		
		ha_msg_add(acknak, CRM_OP_JOINACK, XML_BOOLEAN_FALSE);
	}
	
	send_msg_via_ha(fsa_cluster_conn, acknak);
	return (gboolean)GPOINTER_TO_INT(user_data);
}

void
initialize_join(gboolean before)
{
	/* clear out/reset a bunch of stuff */
	crm_debug("Initializing join data");
	g_hash_table_destroy(confirmed_nodes);
	g_hash_table_destroy(confirmed_nodes);
	g_hash_table_destroy(confirmed_nodes);
	g_hash_table_destroy(confirmed_nodes);

	if(before) {
		if(max_generation_from != NULL) {
			crm_free(max_generation_from);
			max_generation_from = NULL;
		}
		if(max_generation_xml != NULL) {
			free_xml(max_generation_xml);
			max_generation_xml = NULL;
		}
		set_bit_inplace(fsa_input_register, R_HAVE_CIB);
		clear_bit_inplace(fsa_input_register, R_CIB_ASKED);
	}
	
	welcomed_nodes = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_hash_destroy_str, g_hash_destroy_str);
	integrated_nodes = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_hash_destroy_str, g_hash_destroy_str);
	finalized_nodes = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_hash_destroy_str, g_hash_destroy_str);
	confirmed_nodes = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_hash_destroy_str, g_hash_destroy_str);
}


void
join_send_offer(gpointer key, gpointer value, gpointer user_data)
{
	const char *join_to = NULL;
	const oc_node_t *member = (const oc_node_t*)value;

	if(member != NULL) {
		join_to = member->node_uname;
	}

	if(join_to == NULL) {
		crm_err("No recipient for welcome message");

	} else {
		HA_Message *offer = create_request(
			CRM_OP_WELCOME, NULL, join_to,
			CRM_SYSTEM_CRMD, CRM_SYSTEM_DC, NULL);

		/* send the welcome */
		crm_info("Sending %s to %s", CRM_OP_WELCOME, join_to);

		send_msg_via_ha(fsa_cluster_conn, offer);

		g_hash_table_insert(welcomed_nodes, crm_strdup(join_to),
				    crm_strdup(CRMD_JOINSTATE_PENDING));
	}
}

