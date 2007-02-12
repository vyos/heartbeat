/* $Id: stonith.c,v 1.26 2006/04/10 09:07:21 sunjd Exp $ */
/*
 * Stonith API infrastructure.
 *
 * Copyright (c) 2000 Alan Robertson <alanr@unix.sh>
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
#include <portability.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <dlfcn.h>
#include <dirent.h>
#include <glib.h>
#define ENABLE_PIL_DEFS_PRIVATE
#include <pils/plugin.h>
#include <pils/generic.h>
#include <stonith/stonith.h>
#include <stonith/stonith_plugin.h>


#define MALLOC		StonithPIsys->imports->alloc
#define MALLOCT(t)	(t*)(MALLOC(sizeof(t)))
#define REALLOC		StonithPIsys->imports->mrealloc
#define STRDUP		StonithPIsys->imports->mstrdup
#define FREE(p)		{StonithPIsys->imports->mfree(p); (p) = NULL;}

#define	LOG(args...) PILCallLog(StonithPIsys->imports->log, args)

#define EXTPINAME_S 	"external"

PILPluginUniv*		StonithPIsys = NULL;
static GHashTable*	Splugins = NULL;
static int		init_pluginsys(void);
extern StonithImports	stonithimports;

static PILGenericIfMgmtRqst	Reqs[] =
{
	{STONITH_TYPE_S, &Splugins, &stonithimports, NULL, NULL},
	{NULL, NULL, NULL, NULL, NULL}
};

void PILpisysSetDebugLevel(int);
/* Initialize the plugin system... */
static int
init_pluginsys(void) {

	if (StonithPIsys) {
		return TRUE;
	}


	/* PILpisysSetDebugLevel(10); */
	StonithPIsys = NewPILPluginUniv(STONITH_MODULES);
	
	if (StonithPIsys) {
		if (PILLoadPlugin(StonithPIsys, PI_IFMANAGER, "generic", Reqs)
		!=	PIL_OK){
			fprintf(stderr, "generic plugin load failed\n");
			DelPILPluginUniv(StonithPIsys);
			StonithPIsys = NULL;
		}
		/*PILSetDebugLevel(StonithPIsys, PI_IFMANAGER, "generic", 10);*/
	}else{
		fprintf(stderr, "pi univ creation failed\n");
	}
	return StonithPIsys != NULL;
}

/*
 *	Create a new Stonith object of the requested type.
 */

Stonith *
stonith_new(const char * type)
{
	StonithPlugin *		sp = NULL;
	struct stonith_ops*	ops = NULL;
	char *			key;
	char *			subplugin;
	char *			typecopy;


	if (!init_pluginsys()) {
		return NULL;
	}
	
	if ((typecopy = STRDUP(type)) == NULL) {
		return NULL;
	}

	if (((subplugin = strchr(typecopy, '/')) != NULL) && 
	    (strncmp(EXTPINAME_S, typecopy, strlen(EXTPINAME_S)) == 0)) {
		*subplugin++ = 0; /* make two strings */
	}

	/* Look and see if it's already loaded... */

	if (g_hash_table_lookup_extended(Splugins, typecopy
	,	(gpointer)&key, (gpointer)&ops)) {
		/* Yes!  Increment reference count */
		PILIncrIFRefCount(StonithPIsys, STONITH_TYPE_S, typecopy, 1);

	}else{		/* No.  Try and load it... */
		if (PILLoadPlugin(StonithPIsys, STONITH_TYPE_S, typecopy, NULL)
		!=	PIL_OK) {
			FREE(typecopy);
			return NULL;
		}

		/* Look up the plugin in the Splugins table */
		if (!g_hash_table_lookup_extended(Splugins, typecopy
		,		(void*)&key, (void*)&ops)) {
			/* OOPS! didn't find it(!?!)... */
			PILIncrIFRefCount(StonithPIsys, STONITH_TYPE_S
			,	typecopy, -1);
			FREE(typecopy);
			return NULL;
		}
	}

	if (ops != NULL) {
		sp = ops->new((const char *)(subplugin));
		if (sp != NULL) {
			sp->s.stype = STRDUP(typecopy);
		}
	}

	FREE(typecopy);
	return sp ? (&sp->s) : NULL;
}

static int
qsort_string_cmp(const void *a, const void *b)
{
	return(strcmp(*(const char * const *)a, *(const char * const *)b));
}

/*
 *	Return list of STONITH types valid in stonith_new()
 */

char **
stonith_types(void)
{
	int plugincount;
	static char **	lasttypelist = NULL;
	char **		newtypelist;
	int		extplugin = -1;

	if (!init_pluginsys()) {
		return NULL;
	}

	if (lasttypelist) {
		stonith_free_hostlist(lasttypelist);
		lasttypelist = NULL;
	}

	newtypelist = PILListPlugins(StonithPIsys, STONITH_TYPE_S, NULL);
	if (newtypelist == NULL) {
		return NULL;
	}

	/* look for 'external' plugin */
	for (plugincount=0; newtypelist[plugincount] != NULL; ++plugincount) {
		if (strcmp(newtypelist[plugincount], EXTPINAME_S) == 0) {
			extplugin = plugincount;
		}
	}

	if (extplugin >= 0) {
		/* 'external' plugin exists, adjust types list by replacing
		 * 'external' with sorted list of 'external/subplugin' */
		const char **extPI, **p;
		int numextPI, i, index;
		Stonith * ext;

		/* let the external plugin return a list */
		if ((ext = stonith_new(EXTPINAME_S)) == NULL) {
			LOG(PIL_CRIT, "Cannot create new external "
				"plugin object");
			goto types_exit;
		}
		if ((extPI = stonith_get_confignames(ext)) == NULL) {
			LOG(PIL_CRIT, "Cannot get external plugin subplugins");
			stonith_delete(ext);
			goto types_exit;
		}

		/* count the external plugins */
		for (numextPI = 0, p = extPI; *p; p++, numextPI++);

		/* sort the external plugins */
		qsort(extPI, numextPI, sizeof(char *), qsort_string_cmp);

		/* allocate types list (don't need numextPI+plugincount+1
		 * because 'external' is being removed from list */
		lasttypelist = (char **)
			MALLOC((numextPI+plugincount) * sizeof(char *));
		if (lasttypelist == NULL) {
			LOG(PIL_CRIT, "Out of memory");
			stonith_delete(ext);
			goto types_exit;
		}

		memset(lasttypelist, 0, (numextPI+plugincount)*sizeof(char *)); 

		/* copy plugins up to but not including 'external' */
		for (index = 0, i = 0; i < extplugin; index++, i++) {
			lasttypelist[index] = STRDUP(newtypelist[i]);
			if (lasttypelist[index] == NULL) {
				LOG(PIL_CRIT, "Out of memory");
				stonith_delete(ext);
				goto types_exit_mem;
			}
		}

		/* copy external plugins */
		for (i = 0; i < numextPI; index++, i++) {
			int len = strlen(EXTPINAME_S) + 
				strlen(extPI[i]) + 2;
			lasttypelist[index] = MALLOC(len * sizeof(char *));
			if (lasttypelist[index] == NULL) {
				LOG(PIL_CRIT, "Out of memory");
				stonith_delete(ext);
				goto types_exit_mem;
			}
			snprintf(lasttypelist[index], len, "%s/%s"
			,	EXTPINAME_S, extPI[i]);
		}

		/* copy plugins after 'external' */
		for (i = extplugin+1; i < plugincount; index++, i++) {
			lasttypelist[index] = STRDUP(newtypelist[i]);
			if (lasttypelist[index] == NULL) {
				LOG(PIL_CRIT, "Out of memory");
				stonith_delete(ext);
				goto types_exit_mem;
			}
		}

		stonith_delete(ext);
	}else{
		/* 'external' plugin doesn't exist, copy types list */
		char **from, **to;

		lasttypelist = (char**)MALLOC((plugincount+1) * sizeof(char *));
		if (lasttypelist == NULL) {
			LOG(PIL_CRIT, "Out of memory");
			goto types_exit;
		}
	
		for (from = newtypelist, to = lasttypelist
		; *from
		; ++from, ++to) {
			*to = STRDUP(*from);
			if (*to == NULL) {
				LOG(PIL_CRIT, "Out of memory");
				goto types_exit_mem;
			}
		}
		*to = NULL;
	}
	goto types_exit;

types_exit_mem:
	stonith_free_hostlist(lasttypelist);
	lasttypelist = NULL;
types_exit:
	PILFreePluginList(newtypelist);
	return lasttypelist;
}

/* Destroy the STONITH object... */

void
stonith_delete(Stonith *s)
{
	StonithPlugin*	sp = (StonithPlugin*)s;

	if (sp && sp->s_ops) {
		char *	st = sp->s.stype;
		sp->s_ops->destroy(sp);
		PILIncrIFRefCount(StonithPIsys, STONITH_TYPE_S, st, -1);
		/* destroy should not free it */
		FREE(st);
	}
}

const char **
stonith_get_confignames(Stonith* s)
{
	StonithPlugin*	sp = (StonithPlugin*)s;

	if (sp && sp->s_ops) {
		return sp->s_ops->get_confignames(sp);
	}
	return NULL;
}

const char*
stonith_get_info(Stonith* s, int infotype)
{
	StonithPlugin*	sp = (StonithPlugin*)s;

	if (sp && sp->s_ops) {
		return sp->s_ops->get_info(sp, infotype);
	}
	return NULL;

}

void
stonith_set_debug	(Stonith* s, int debuglevel)
{
	StonithPlugin*	sp = (StonithPlugin*)s;
	if (StonithPIsys == NULL) {
		return;
	}
	PILSetDebugLevel(StonithPIsys, STONITH_TYPE_S, sp->s.stype, debuglevel);
}

void
stonith_set_log(Stonith* s, PILLogFun logfun)
{
	if (StonithPIsys == NULL) {
		return;
	}
	PilPluginUnivSetLog(StonithPIsys, logfun);
}

int
stonith_set_config(Stonith* s, StonithNVpair* list)
{
	StonithPlugin*	sp = (StonithPlugin*)s;

	if (sp && sp->s_ops) {
		int	rc = sp->s_ops->set_config(sp, list);
		if (rc == S_OK) {
			sp->isconfigured = TRUE;
		}
		return rc;
	}
	return S_INVAL;
}

/*
 * FIXME: We really ought to support files with name=value type syntax
 * on each line...
 *
 */
int
stonith_set_config_file(Stonith* s, const char * configname)
{
	FILE *		cfgfile;

	char		line[1024];

	if ((cfgfile = fopen(configname, "r")) == NULL)  {
		LOG(PIL_CRIT, "Cannot open %s", configname);
		return(S_BADCONFIG);
	}
	while (fgets(line, sizeof(line), cfgfile) != NULL){
		int	len;
		
		if (*line == '#' || *line == '\n' || *line == EOS) {
			continue;
		}
		
		/*remove the new line in the end*/
		len = strnlen(line, sizeof(line)-1);
		if (line[len-1] == '\n'){
			line[len-1] = '\0';
		}else{
			line[len] = '\0';
		}
	
		fclose(cfgfile);
		return stonith_set_config_info(s, line);
	}
	fclose(cfgfile);
	return S_BADCONFIG;
}

int
stonith_set_config_info(Stonith* s, const char * info)
{
	StonithNVpair*	cinfo;
	int		rc;
	cinfo = stonith1_compat_string_to_NVpair(s, info);
	if (cinfo == NULL) {
		return S_BADCONFIG;
	}
	rc = stonith_set_config(s, cinfo);
	free_NVpair(cinfo); cinfo = NULL;
	return rc;
}

char**
stonith_get_hostlist(Stonith* s)
{
	StonithPlugin*	sp = (StonithPlugin*)s;
	if (sp && sp->s_ops && sp->isconfigured) {
		return sp->s_ops->get_hostlist(sp);
	}
	return NULL;
}

void
stonith_free_hostlist(char** hostlist)
{
	char ** here;

	for (here=hostlist; *here; ++here) {
		FREE(*here);
	}
	FREE(hostlist);
}

int
stonith_get_status(Stonith* s)
{
	StonithPlugin*	sp = (StonithPlugin*)s;
	if (sp && sp->s_ops && sp->isconfigured) {
		return sp->s_ops->get_status(sp);
	}
	return S_INVAL;
}

int
stonith_req_reset(Stonith* s, int operation, const char* node)
{
	StonithPlugin*	sp = (StonithPlugin*)s;
	if (sp && sp->s_ops && sp->isconfigured) {
		char*		nodecopy = STRDUP(node);
		int		rc;
		if (nodecopy == NULL) {
			return S_OOPS;
		}
		g_strdown(nodecopy);

		rc = sp->s_ops->req_reset(sp, operation, nodecopy);
		FREE(nodecopy);
		return rc;
	}
	return S_INVAL;
}
/* Stonith 1 compatibility:  Convert a string to an NVpair set */
StonithNVpair*
stonith1_compat_string_to_NVpair(Stonith* s, const char * str)
{
	/* We make some assumptions that the order of parameters in the
	 * result from stonith_get_confignames() matches that which
	 * was required from a Stonith1 module.
	 * Everything after the last delimiter is passed along as part of
	 * the final argument - white space and all...
	 */
	const char **	config_names;
	int		n_names;
	int		j;
	const char *	delims = " \t\n\r\f";
	StonithNVpair*	ret;

	if ((config_names = stonith_get_confignames(s)) == NULL) {
		return NULL;
	}
	for (n_names=0; config_names[n_names] != NULL; ++n_names) {
		/* Just count */;
	}
	ret = (StonithNVpair*) (MALLOC((n_names+1)*sizeof(StonithNVpair)));
	if (ret == NULL) {
		return NULL;
	}
	memset(ret, 0, (n_names+1)*sizeof(StonithNVpair));
	for (j=0; j < n_names; ++j) {
		size_t	len;
		if ((ret[j].s_name = STRDUP(config_names[j])) == NULL) {
			goto freeandexit;
		}
		ret[j].s_value = NULL;
		str += strspn(str, delims);
		if (*str == EOS) {
			goto freeandexit;
		}
		if (j == (n_names -1)) {
			len = strlen(str);
		}else{
			len = strcspn(str, delims);
		}
		if ((ret[j].s_value = MALLOC((len+1)*sizeof(char))) == NULL) {
			goto freeandexit;
		}
		memcpy(ret[j].s_value, str, len);
		ret[j].s_value[len] = EOS;
		str += len;
	}
	ret[j].s_name = NULL;
	return ret;
freeandexit:
	free_NVpair(ret); ret = NULL;
	return NULL;
}

static int NVcur = -1;
static int NVmax = -1;
static gboolean NVerr = FALSE;

static void
stonith_walk_ghash(gpointer key, gpointer value, gpointer user_data)
{
	StonithNVpair*	u = user_data;
	
	if (NVcur <= NVmax && !NVerr) {
		u[NVcur].s_name = STRDUP(key);
		u[NVcur].s_value = STRDUP(value);
		if (u[NVcur].s_name == NULL || u[NVcur].s_value == NULL) {
			/* Memory allocation error */
			NVerr = TRUE;
			return;
		}
		++NVcur;
	}else{
		NVerr = TRUE;
	}
}


StonithNVpair*
stonith_ghash_to_NVpair(GHashTable* stringtable)
{
	int		hsize = g_hash_table_size(stringtable);
	StonithNVpair*	ret;

	if ((ret = (StonithNVpair*)MALLOC(sizeof(StonithNVpair)*(hsize+1))) == NULL) {
		return NULL;
	}
	NVmax = hsize;
	NVcur = 0;
	ret[hsize].s_name = NULL;
	ret[hsize].s_value = NULL;
	g_hash_table_foreach(stringtable, stonith_walk_ghash, ret);
	NVmax = NVcur = -1;
	if (NVerr) {
		free_NVpair(ret);
		ret = NULL;
	}
	return ret;
}

void
free_NVpair(StonithNVpair* nv)
{
	StonithNVpair* this;

	if (nv == NULL) {
		return;
	}
	for (this=nv; this->s_name; ++this) {
		FREE(this->s_name);
		if (this->s_value) {
			FREE(this->s_value);
		}
	}
	FREE(nv);
}
