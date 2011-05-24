/**
 *  Copyright 2004-2005 Paul Querna
 *  Copyright 2007 Nikos Mavrogiannopoulos
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "mod_gnutls.h"
#include "http_vhost.h"
#include "ap_mpm.h"

#if APR_HAS_THREADS
# if GNUTLS_VERSION_MAJOR <= 2 && GNUTLS_VERSION_MINOR < 11
#include <gcrypt.h>
GCRY_THREAD_OPTION_PTHREAD_IMPL;
# endif
#endif

#if !USING_2_1_RECENT
extern server_rec *ap_server_conf;
#endif

#ifdef ENABLE_SRP
#include "apr_base64.h"
#endif

#if MOD_GNUTLS_DEBUG
static apr_file_t *debug_log_fp;
#endif

static int mpm_is_threaded;
static gnutls_datum session_ticket_key = { NULL, 0 };

static int mgs_cert_verify(request_rec * r, mgs_handle_t * ctxt);
/* use side==0 for server and side==1 for client */
static void mgs_add_common_cert_vars(request_rec * r,
				     gnutls_x509_crt_t cert, int side,
				     int export_certificates_enabled);
static void mgs_add_common_pgpcert_vars(request_rec * r,
					gnutls_openpgp_crt_t cert,
					int side,
					int export_certificates_enabled);

/* GnuTLS Callbacks */
#ifdef ENABLE_SRP
static int mgs_srp_server_credentials(gnutls_session_t session,
                                      const char * username,
                                      gnutls_datum_t * salt,
                                      gnutls_datum_t * verifier,
                                      gnutls_datum_t * g, gnutls_datum_t * n);
#endif

static apr_status_t mgs_cleanup_pre_config(void *data)
{
	gnutls_free(session_ticket_key.data);
	session_ticket_key.data = NULL;
	session_ticket_key.size = 0;
	gnutls_global_deinit();
	return APR_SUCCESS;
}

#if MOD_GNUTLS_DEBUG
static void gnutls_debug_log_all(int level, const char *str)
{
	apr_file_printf(debug_log_fp, "<%d> %s\n", level, str);
}

#define _gnutls_log apr_file_printf
#else
# define _gnutls_log(...)
#endif

int
mgs_hook_pre_config(apr_pool_t * pconf,
		    apr_pool_t * plog, apr_pool_t * ptemp)
{
	int ret;

#if MOD_GNUTLS_DEBUG
	apr_file_open(&debug_log_fp, "/tmp/gnutls_debug",
		      APR_APPEND | APR_WRITE | APR_CREATE, APR_OS_DEFAULT,
		      pconf);

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);

	gnutls_global_set_log_level(9);
	gnutls_global_set_log_function(gnutls_debug_log_all);
	_gnutls_log(debug_log_fp, "gnutls: %s\n",
		    gnutls_check_version(NULL));
#endif

#if APR_HAS_THREADS
	ap_mpm_query(AP_MPMQ_IS_THREADED, &mpm_is_threaded);
#if (GNUTLS_VERSION_MAJOR == 2 && GNUTLS_VERSION_MINOR < 11) || GNUTLS_VERSION_MAJOR < 2
	if (mpm_is_threaded) {
		gcry_control(GCRYCTL_SET_THREAD_CBS,
			     &gcry_threads_pthread);
	}
#endif
#else
	mpm_is_threaded = 0;
#endif


	if (gnutls_check_version(LIBGNUTLS_VERSION) == NULL) {
		_gnutls_log(debug_log_fp,
			    "gnutls_check_version() failed. Required: gnutls-%s Found: gnutls-%s\n",
			    LIBGNUTLS_VERSION, gnutls_check_version(NULL));
		return -3;
	}

	ret = gnutls_global_init();
	if (ret < 0) {
		_gnutls_log(debug_log_fp, "gnutls_global_init: %s\n",
			    gnutls_strerror(ret));
		return -3;
	}

	ret = gnutls_session_ticket_key_generate(&session_ticket_key);
	if (ret < 0) {
		_gnutls_log(debug_log_fp,
			    "gnutls_session_ticket_key_generate: %s\n",
			    gnutls_strerror(ret));
	}

	apr_pool_cleanup_register(pconf, NULL, mgs_cleanup_pre_config,
				  apr_pool_cleanup_null);


	return OK;
}

static int mgs_select_virtual_server_cb(gnutls_session_t session)
{
	mgs_handle_t *ctxt;
	mgs_srvconf_rec *tsc;
	int ret;
	int cprio[2];

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);

	ctxt = gnutls_transport_get_ptr(session);

	/* find the virtual server */
	tsc = mgs_find_sni_server(session);

	if (tsc != NULL)
		ctxt->sc = tsc;

	gnutls_certificate_server_set_request(session,
					      ctxt->
					      sc->client_verify_mode);

	/* set the new server credentials 
	 */

	gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE,
			       ctxt->sc->certs);

	gnutls_credentials_set(session, GNUTLS_CRD_ANON,
			       ctxt->sc->anon_creds);

#ifdef ENABLE_SRP
	if (ctxt->sc->srp_tpasswd_conf_file != NULL
	    && (ctxt->sc->srp_tpasswd_file != NULL
                || ctxt->sc->srp_passwd_query != NULL)) {
		gnutls_credentials_set(session, GNUTLS_CRD_SRP,
				       ctxt->sc->srp_creds);
	}
#endif

	/* update the priorities - to avoid negotiating a ciphersuite that is not
	 * enabled on this virtual server. Note that here we ignore the version
	 * negotiation.
	 */
	ret = gnutls_priority_set(session, ctxt->sc->priorities);
	/* actually it shouldn't fail since we have checked at startup */
	if (ret < 0)
		return ret;

	/* If both certificate types are not present disallow them from
	 * being negotiated.
	 */
	if (ctxt->sc->certs_x509[0] != NULL && ctxt->sc->cert_pgp == NULL) {
		cprio[0] = GNUTLS_CRT_X509;
		cprio[1] = 0;
		gnutls_certificate_type_set_priority(session, cprio);
	} else if (ctxt->sc->cert_pgp != NULL
		   && ctxt->sc->certs_x509[0] == NULL) {
		cprio[0] = GNUTLS_CRT_OPENPGP;
		cprio[1] = 0;
		gnutls_certificate_type_set_priority(session, cprio);
	}

	return 0;
}

static int cert_retrieve_fn(gnutls_session_t session, gnutls_retr_st * ret)
{
	mgs_handle_t *ctxt;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	ctxt = gnutls_transport_get_ptr(session);

	if (ctxt == NULL)
		return GNUTLS_E_INTERNAL_ERROR;

	if (gnutls_certificate_type_get(session) == GNUTLS_CRT_X509) {
		ret->type = GNUTLS_CRT_X509;
		ret->ncerts = ctxt->sc->certs_x509_num;
		ret->deinit_all = 0;

		ret->cert.x509 = ctxt->sc->certs_x509;
		ret->key.x509 = ctxt->sc->privkey_x509;

		return 0;
	} else if (gnutls_certificate_type_get(session) ==
		   GNUTLS_CRT_OPENPGP) {
		ret->type = GNUTLS_CRT_OPENPGP;
		ret->ncerts = 1;
		ret->deinit_all = 0;

		ret->cert.pgp = ctxt->sc->cert_pgp;
		ret->key.pgp = ctxt->sc->privkey_pgp;

		return 0;

	}

	return GNUTLS_E_INTERNAL_ERROR;
}

/* 2048-bit group parameters from SRP specification */
const char static_dh_params[] = "-----BEGIN DH PARAMETERS-----\n"
    "MIIBBwKCAQCsa9tBMkqam/Fm3l4TiVgvr3K2ZRmH7gf8MZKUPbVgUKNzKcu0oJnt\n"
    "gZPgdXdnoT3VIxKrSwMxDc1/SKnaBP1Q6Ag5ae23Z7DPYJUXmhY6s2YaBfvV+qro\n"
    "KRipli8Lk7hV+XmT7Jde6qgNdArb9P90c1nQQdXDPqcdKB5EaxR3O8qXtDoj+4AW\n"
    "dr0gekNsZIHx0rkHhxdGGludMuaI+HdIVEUjtSSw1X1ep3onddLs+gMs+9v1L7N4\n"
    "YWAnkATleuavh05zA85TKZzMBBx7wwjYKlaY86jQw4JxrjX46dv7tpS1yAPYn3rk\n"
    "Nd4jbVJfVHWbZeNy/NaO8g+nER+eSv9zAgEC\n"
    "-----END DH PARAMETERS-----\n";

/* Read the common name or the alternative name of the certificate.
 * We only support a single name per certificate.
 *
 * Returns negative on error.
 */
static int read_crt_cn(server_rec * s, apr_pool_t * p,
		       gnutls_x509_crt_t cert, char **cert_cn)
{
	int rv = 0, i;
	size_t data_len;


	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	*cert_cn = NULL;

	data_len = 0;
	rv = gnutls_x509_crt_get_dn_by_oid(cert,
					   GNUTLS_OID_X520_COMMON_NAME,
					   0, 0, NULL, &data_len);

	if (rv == GNUTLS_E_SHORT_MEMORY_BUFFER && data_len > 1) {
		*cert_cn = apr_palloc(p, data_len);
		rv = gnutls_x509_crt_get_dn_by_oid(cert,
						   GNUTLS_OID_X520_COMMON_NAME,
						   0, 0, *cert_cn,
						   &data_len);
	} else {		/* No CN return subject alternative name */
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
			     "No common name found in certificate for '%s:%d'. Looking for subject alternative name...",
			     s->server_hostname, s->port);
		rv = 0;
		/* read subject alternative name */
		for (i = 0; !(rv < 0); i++) {
			data_len = 0;
			rv = gnutls_x509_crt_get_subject_alt_name(cert, i,
								  NULL,
								  &data_len,
								  NULL);

			if (rv == GNUTLS_E_SHORT_MEMORY_BUFFER
			    && data_len > 1) {
				/* FIXME: not very efficient. What if we have several alt names
				 * before DNSName?
				 */
				*cert_cn = apr_palloc(p, data_len + 1);

				rv = gnutls_x509_crt_get_subject_alt_name
				    (cert, i, *cert_cn, &data_len, NULL);
				(*cert_cn)[data_len] = 0;

				if (rv == GNUTLS_SAN_DNSNAME)
					break;
			}
		}
	}

	return rv;
}

static int read_pgpcrt_cn(server_rec * s, apr_pool_t * p,
			  gnutls_openpgp_crt_t cert, char **cert_cn)
{
	int rv = 0;
	size_t data_len;


	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	*cert_cn = NULL;

	data_len = 0;
	rv = gnutls_openpgp_crt_get_name(cert, 0, NULL, &data_len);

	if (rv == GNUTLS_E_SHORT_MEMORY_BUFFER && data_len > 1) {
		*cert_cn = apr_palloc(p, data_len);
		rv = gnutls_openpgp_crt_get_name(cert, 0, *cert_cn,
						 &data_len);
	} else {		/* No CN return subject alternative name */
		ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
			     "No name found in PGP certificate for '%s:%d'.",
			     s->server_hostname, s->port);
	}

	return rv;
}


int
mgs_hook_post_config(apr_pool_t * p, apr_pool_t * plog,
		     apr_pool_t * ptemp, server_rec * base_server)
{
	int rv;
	server_rec *s;
	gnutls_dh_params_t dh_params = NULL;
	gnutls_rsa_params_t rsa_params = NULL;
	mgs_srvconf_rec *sc;
	mgs_srvconf_rec *sc_base;
	void *data = NULL;
	int first_run = 0;
	const char *userdata_key = "mgs_init";

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	apr_pool_userdata_get(&data, userdata_key,
			      base_server->process->pool);
	if (data == NULL) {
		first_run = 1;
		apr_pool_userdata_set((const void *) 1, userdata_key,
				      apr_pool_cleanup_null,
				      base_server->process->pool);
	}


	s = base_server;
	sc_base =
	    (mgs_srvconf_rec *) ap_get_module_config(s->module_config,
						     &gnutls_module);

	gnutls_dh_params_init(&dh_params);

	if (sc_base->dh_params == NULL) {
		gnutls_datum pdata = {
			(void *) static_dh_params,
			sizeof(static_dh_params)
		};
		/* loading defaults */
		rv = gnutls_dh_params_import_pkcs3(dh_params, &pdata,
						   GNUTLS_X509_FMT_PEM);

		if (rv < 0) {
			ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
				     "GnuTLS: Unable to load DH Params: (%d) %s",
				     rv, gnutls_strerror(rv));
			exit(rv);
		}
	} else
		dh_params = sc_base->dh_params;

	if (sc_base->rsa_params != NULL)
		rsa_params = sc_base->rsa_params;

	/* else not an error but RSA-EXPORT ciphersuites are not available 
	 */

	rv = mgs_cache_post_config(p, s, sc_base);
	if (rv != 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, rv, s,
			     "GnuTLS: Post Config for GnuTLSCache Failed."
			     " Shutting Down.");
		exit(-1);
	}

	for (s = base_server; s; s = s->next) {
		void *load = NULL;
		sc = (mgs_srvconf_rec *)
		    ap_get_module_config(s->module_config, &gnutls_module);
		sc->cache_type = sc_base->cache_type;
		sc->cache_config = sc_base->cache_config;

		/* Check if the priorities have been set */
		if (sc->priorities == NULL
		    && sc->enabled == GNUTLS_ENABLED_TRUE) {
			ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
				     "GnuTLS: Host '%s:%d' is missing the GnuTLSPriorities directive!",
				     s->server_hostname, s->port);
			exit(-1);
		}

		/* Check if DH or RSA params have been set per host */
		if (sc->rsa_params != NULL)
			load = sc->rsa_params;
		else if (rsa_params)
			load = rsa_params;

		if (load != NULL)
			gnutls_certificate_set_rsa_export_params(sc->certs,
								 load);


		load = NULL;
		if (sc->dh_params != NULL)
			load = sc->dh_params;
		else if (dh_params)
			load = dh_params;

		if (load != NULL) {	/* not needed but anyway */
			gnutls_certificate_set_dh_params(sc->certs, load);
			gnutls_anon_set_server_dh_params(sc->anon_creds,
							 load);
		}

		gnutls_certificate_server_set_retrieve_function(sc->certs,
								cert_retrieve_fn);

#ifdef ENABLE_SRP
		if (sc->srp_tpasswd_conf_file != NULL
                    && sc->srp_tpasswd_file != NULL) {
                        rv = gnutls_srp_set_server_credentials_file
                                (sc->srp_creds, sc->srp_tpasswd_file,
                                 sc->srp_tpasswd_conf_file);

			if (rv < 0 && sc->enabled == GNUTLS_ENABLED_TRUE) {
				ap_log_error(APLOG_MARK, APLOG_STARTUP, 0,
					     s,
					     "[GnuTLS] - Host '%s:%d' is missing "
					     "SRP passwd or conf File!",
					     s->server_hostname, s->port);
				exit(-1);
			}
                } else if (sc->srp_passwd_query != NULL) {
                        gnutls_srp_set_server_credentials_function
                                (sc->srp_creds, &mgs_srp_server_credentials);
		}
#endif

		if (sc->certs_x509[0] == NULL &&
		    sc->cert_pgp == NULL &&
		    sc->enabled == GNUTLS_ENABLED_TRUE) {
			ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
				     "[GnuTLS] - Host '%s:%d' is missing a "
				     "Certificate File!",
				     s->server_hostname, s->port);
			exit(-1);
		}

		if (sc->enabled == GNUTLS_ENABLED_TRUE &&
		    ((sc->certs_x509[0] != NULL
		      && sc->privkey_x509 == NULL) || (sc->cert_pgp != NULL
						       && sc->privkey_pgp
						       == NULL))) {
			ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
				     "[GnuTLS] - Host '%s:%d' is missing a "
				     "Private Key File!",
				     s->server_hostname, s->port);
			exit(-1);
		}

		if (sc->enabled == GNUTLS_ENABLED_TRUE) {
			rv = read_crt_cn(s, p, sc->certs_x509[0],
					 &sc->cert_cn);
			if (rv < 0 && sc->cert_pgp != NULL)	/* try openpgp certificate */
				rv = read_pgpcrt_cn(s, p, sc->cert_pgp,
						    &sc->cert_cn);

			if (rv < 0) {
				ap_log_error(APLOG_MARK, APLOG_STARTUP, 0,
					     s,
					     "[GnuTLS] - Cannot find a certificate for host '%s:%d'!",
					     s->server_hostname, s->port);
				sc->cert_cn = NULL;
				continue;
			}
		}
	}


	ap_add_version_component(p, "mod_gnutls/" MOD_GNUTLS_VERSION);

	return OK;
}

void mgs_hook_child_init(apr_pool_t * p, server_rec * s)
{
	apr_status_t rv = APR_SUCCESS;
	mgs_srvconf_rec *sc = ap_get_module_config(s->module_config,
						   &gnutls_module);

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	if (sc->cache_type != mgs_cache_none) {
		rv = mgs_cache_child_init(p, s, sc);
		if (rv != APR_SUCCESS) {
			ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s,
				     "[GnuTLS] - Failed to run Cache Init");
		}
	}
}

const char *mgs_hook_http_scheme(const request_rec * r)
{
	mgs_srvconf_rec *sc;

	if (r == NULL)
		return NULL;

	sc = (mgs_srvconf_rec *) ap_get_module_config(r->
						      server->module_config,
						      &gnutls_module);

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	if (sc->enabled == GNUTLS_ENABLED_FALSE) {
		return NULL;
	}

	return "https";
}

apr_port_t mgs_hook_default_port(const request_rec * r)
{
	mgs_srvconf_rec *sc;

	if (r == NULL)
		return 0;

	sc = (mgs_srvconf_rec *) ap_get_module_config(r->
						      server->module_config,
						      &gnutls_module);

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	if (sc->enabled == GNUTLS_ENABLED_FALSE) {
		return 0;
	}

	return 443;
}

#define MAX_HOST_LEN 255

#if USING_2_1_RECENT
typedef struct {
	mgs_handle_t *ctxt;
	mgs_srvconf_rec *sc;
	const char *sni_name;
} vhost_cb_rec;

static int vhost_cb(void *baton, conn_rec * conn, server_rec * s)
{
	mgs_srvconf_rec *tsc;
	vhost_cb_rec *x = baton;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	tsc = (mgs_srvconf_rec *) ap_get_module_config(s->module_config,
						       &gnutls_module);

	if (tsc->enabled != GNUTLS_ENABLED_TRUE || tsc->cert_cn == NULL) {
		return 0;
	}

	/* The CN can contain a * -- this will match those too. */
	if (ap_strcasecmp_match(x->sni_name, tsc->cert_cn) == 0) {
		/* found a match */
#if MOD_GNUTLS_DEBUG
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
			     x->ctxt->c->base_server,
			     "GnuTLS: Virtual Host CB: "
			     "'%s' == '%s'", tsc->cert_cn, x->sni_name);
#endif
		/* Because we actually change the server used here, we need to reset
		 * things like ClientVerify.
		 */
		x->sc = tsc;
		/* Shit. Crap. Dammit. We *really* should rehandshake here, as our
		 * certificate structure *should* change when the server changes. 
		 * acccckkkkkk. 
		 */
		return 1;
	} else {
#if MOD_GNUTLS_DEBUG
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
			     x->ctxt->c->base_server,
			     "GnuTLS: Virtual Host CB: "
			     "'%s' != '%s'", tsc->cert_cn, x->sni_name);
#endif

	}
	return 0;
}
#endif

mgs_srvconf_rec *mgs_find_sni_server(gnutls_session_t session)
{
	int rv;
	unsigned int sni_type;
	size_t data_len = MAX_HOST_LEN;
	char sni_name[MAX_HOST_LEN];
	mgs_handle_t *ctxt;
#if USING_2_1_RECENT
	vhost_cb_rec cbx;
#else
	server_rec *s;
	mgs_srvconf_rec *tsc;
#endif

	if (session == NULL)
		return NULL;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	ctxt = gnutls_transport_get_ptr(session);

	rv = gnutls_server_name_get(ctxt->session, sni_name,
				    &data_len, &sni_type, 0);

	if (rv != 0) {
		return NULL;
	}

	if (sni_type != GNUTLS_NAME_DNS) {
		ap_log_error(APLOG_MARK, APLOG_CRIT, 0,
			     ctxt->c->base_server,
			     "GnuTLS: Unknown type '%d' for SNI: "
			     "'%s'", sni_type, sni_name);
		return NULL;
	}

    /**
     * Code in the Core already sets up the c->base_server as the base
     * for this IP/Port combo.  Trust that the core did the 'right' thing.
     */
#if USING_2_1_RECENT
	cbx.ctxt = ctxt;
	cbx.sc = NULL;
	cbx.sni_name = sni_name;

	rv = ap_vhost_iterate_given_conn(ctxt->c, vhost_cb, &cbx);
	if (rv == 1) {
		return cbx.sc;
	}
#else
	for (s = ap_server_conf; s; s = s->next) {

		tsc =
		    (mgs_srvconf_rec *)
		    ap_get_module_config(s->module_config, &gnutls_module);
		if (tsc->enabled != GNUTLS_ENABLED_TRUE) {
			continue;
		}
#if MOD_GNUTLS_DEBUG
		ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
			     ctxt->c->base_server,
			     "GnuTLS: sni-x509 cn: %s/%d pk: %s s: 0x%08X s->n: 0x%08X  sc: 0x%08X",
			     tsc->cert_cn, rv,
			     gnutls_pk_algorithm_get_name
			     (gnutls_x509_privkey_get_pk_algorithm
			      (ctxt->sc->privkey_x509)), (unsigned int) s,
			     (unsigned int) s->next, (unsigned int) tsc);
#endif
		/* The CN can contain a * -- this will match those too. */
		if (ap_strcasecmp_match(sni_name, tsc->cert_cn) == 0) {
#if MOD_GNUTLS_DEBUG
			ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
				     ctxt->c->base_server,
				     "GnuTLS: Virtual Host: "
				     "'%s' == '%s'", tsc->cert_cn,
				     sni_name);
#endif
			return tsc;
		}
	}
#endif
	return NULL;
}


static const int protocol_priority[] = {
	GNUTLS_TLS1_1, GNUTLS_TLS1_0, GNUTLS_SSL3, 0
};


static mgs_handle_t *create_gnutls_handle(apr_pool_t * pool, conn_rec * c)
{
	mgs_handle_t *ctxt;
	mgs_srvconf_rec *sc =
	    (mgs_srvconf_rec *) ap_get_module_config(c->base_server->
						     module_config,
						     &gnutls_module);

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	ctxt = apr_pcalloc(pool, sizeof(*ctxt));
	ctxt->c = c;
	ctxt->sc = sc;
	ctxt->status = 0;

	ctxt->input_rc = APR_SUCCESS;
	ctxt->input_bb = apr_brigade_create(c->pool, c->bucket_alloc);
	ctxt->input_cbuf.length = 0;

	ctxt->output_rc = APR_SUCCESS;
	ctxt->output_bb = apr_brigade_create(c->pool, c->bucket_alloc);
	ctxt->output_blen = 0;
	ctxt->output_length = 0;

	gnutls_init(&ctxt->session, GNUTLS_SERVER);
	if (session_ticket_key.data != NULL && ctxt->sc->tickets != 0)
		gnutls_session_ticket_enable_server(ctxt->session,
						    &session_ticket_key);

	/* because we don't set any default priorities here (we set later at
	 * the user hello callback) we need to at least set this in order for
	 * gnutls to be able to read packets.
	 */
	gnutls_protocol_set_priority(ctxt->session, protocol_priority);

	gnutls_handshake_set_post_client_hello_function(ctxt->session,
							mgs_select_virtual_server_cb);

	mgs_cache_session_init(ctxt);

	return ctxt;
}

int mgs_hook_pre_connection(conn_rec * c, void *csd)
{
	mgs_handle_t *ctxt;
	mgs_srvconf_rec *sc;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);

	if (c == NULL)
		return DECLINED;

	sc = (mgs_srvconf_rec *) ap_get_module_config(c->base_server->
						      module_config,
						      &gnutls_module);

	if (!(sc && (sc->enabled == GNUTLS_ENABLED_TRUE))) {
		return DECLINED;
	}

	if (c->remote_addr->hostname)
		/* Connection initiated by Apache (mod_proxy) => ignore */
		return OK;

	ctxt = create_gnutls_handle(c->pool, c);

	ap_set_module_config(c->conn_config, &gnutls_module, ctxt);

	gnutls_transport_set_pull_function(ctxt->session,
					   mgs_transport_read);
	gnutls_transport_set_push_function(ctxt->session,
					   mgs_transport_write);
	gnutls_transport_set_ptr(ctxt->session, ctxt);

	ctxt->input_filter =
	    ap_add_input_filter(GNUTLS_INPUT_FILTER_NAME, ctxt, NULL, c);
	ctxt->output_filter =
	    ap_add_output_filter(GNUTLS_OUTPUT_FILTER_NAME, ctxt, NULL, c);

	return OK;
}

int mgs_hook_fixups(request_rec * r)
{
	unsigned char sbuf[GNUTLS_MAX_SESSION_ID];
	char buf[AP_IOBUFSIZE];
	const char *tmp;
	size_t len;
	mgs_handle_t *ctxt;
	int rv = OK;

	if (r == NULL)
		return DECLINED;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	apr_table_t *env = r->subprocess_env;

	ctxt =
	    ap_get_module_config(r->connection->conn_config,
				 &gnutls_module);

	if (!ctxt || ctxt->session == NULL) {
		return DECLINED;
	}

	apr_table_setn(env, "HTTPS", "on");

	apr_table_setn(env, "SSL_VERSION_LIBRARY",
		       "GnuTLS/" LIBGNUTLS_VERSION);
	apr_table_setn(env, "SSL_VERSION_INTERFACE",
		       "mod_gnutls/" MOD_GNUTLS_VERSION);

	apr_table_setn(env, "SSL_PROTOCOL",
		       gnutls_protocol_get_name(gnutls_protocol_get_version
						(ctxt->session)));

	/* should have been called SSL_CIPHERSUITE instead */
	apr_table_setn(env, "SSL_CIPHER",
		       gnutls_cipher_suite_get_name(gnutls_kx_get
						    (ctxt->session),
						    gnutls_cipher_get
						    (ctxt->session),
						    gnutls_mac_get
						    (ctxt->session)));

	apr_table_setn(env, "SSL_COMPRESS_METHOD",
		       gnutls_compression_get_name(gnutls_compression_get
						   (ctxt->session)));

#ifdef ENABLE_SRP
	tmp = gnutls_srp_server_get_username(ctxt->session);
	apr_table_setn(env, "SSL_SRP_USER", (tmp != NULL) ? tmp : "");
#endif

	if (apr_table_get(env, "SSL_CLIENT_VERIFY") == NULL)
		apr_table_setn(env, "SSL_CLIENT_VERIFY", "NONE");

	unsigned int key_size =
	    8 *
	    gnutls_cipher_get_key_size(gnutls_cipher_get(ctxt->session));
	tmp = apr_psprintf(r->pool, "%u", key_size);

	apr_table_setn(env, "SSL_CIPHER_USEKEYSIZE", tmp);

	apr_table_setn(env, "SSL_CIPHER_ALGKEYSIZE", tmp);

	apr_table_setn(env, "SSL_CIPHER_EXPORT",
		       (key_size <= 40) ? "true" : "false");

	len = sizeof(sbuf);
	gnutls_session_get_id(ctxt->session, sbuf, &len);
	tmp = mgs_session_id2sz(sbuf, len, buf, sizeof(buf));
	apr_table_setn(env, "SSL_SESSION_ID", apr_pstrdup(r->pool, tmp));

	if (gnutls_certificate_type_get(ctxt->session) == GNUTLS_CRT_X509)
		mgs_add_common_cert_vars(r, ctxt->sc->certs_x509[0], 0,
					 ctxt->
					 sc->export_certificates_enabled);
	else if (gnutls_certificate_type_get(ctxt->session) ==
		 GNUTLS_CRT_OPENPGP)
		mgs_add_common_pgpcert_vars(r, ctxt->sc->cert_pgp, 0,
					    ctxt->
					    sc->export_certificates_enabled);

	return rv;
}

int mgs_hook_authz(request_rec * r)
{
	int rv;
	mgs_handle_t *ctxt;
	mgs_dirconf_rec *dc;

	if (r == NULL)
		return DECLINED;

	dc = ap_get_module_config(r->per_dir_config, &gnutls_module);

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	ctxt =
	    ap_get_module_config(r->connection->conn_config,
				 &gnutls_module);

	if (!ctxt || ctxt->session == NULL) {
		return DECLINED;
	}

	if (dc->client_verify_mode == GNUTLS_CERT_IGNORE) {
		ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
			      "GnuTLS: Directory set to Ignore Client Certificate!");
	} else {
		if (ctxt->sc->client_verify_mode < dc->client_verify_mode) {
			ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
				      "GnuTLS: Attempting to rehandshake with peer. %d %d",
				      ctxt->sc->client_verify_mode,
				      dc->client_verify_mode);

			/* If we already have a client certificate, there's no point in
			 * re-handshaking... */
			rv = mgs_cert_verify(r, ctxt);
			if (rv != DECLINED && rv != HTTP_FORBIDDEN)
				return rv;

			gnutls_certificate_server_set_request
			    (ctxt->session, dc->client_verify_mode);

			if (mgs_rehandshake(ctxt) != 0) {
				return HTTP_FORBIDDEN;
			}
		} else if (ctxt->sc->client_verify_mode ==
			   GNUTLS_CERT_IGNORE) {
#if MOD_GNUTLS_DEBUG
			ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
				      "GnuTLS: Peer is set to IGNORE");
#endif
			return DECLINED;
		}
		rv = mgs_cert_verify(r, ctxt);
		if (rv != DECLINED &&
		    (rv != HTTP_FORBIDDEN ||
		     dc->client_verify_mode == GNUTLS_CERT_REQUIRE)) {
			return rv;
		}
	}

	return DECLINED;
}

/* variables that are not sent by default:
 *
 * SSL_CLIENT_CERT 	string 	PEM-encoded client certificate
 * SSL_SERVER_CERT 	string 	PEM-encoded client certificate
 */

/* side is either 0 for SERVER or 1 for CLIENT
 */
#define MGS_SIDE ((side==0)?"SSL_SERVER":"SSL_CLIENT")
static void
mgs_add_common_cert_vars(request_rec * r, gnutls_x509_crt_t cert, int side,
			 int export_certificates_enabled)
{
	unsigned char sbuf[64];	/* buffer to hold serials */
	char buf[AP_IOBUFSIZE];
	const char *tmp;
	char *tmp2;
	size_t len;
	int ret, i;

	if (r == NULL)
		return;

	apr_table_t *env = r->subprocess_env;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	if (export_certificates_enabled != 0) {
		char cert_buf[10 * 1024];
		len = sizeof(cert_buf);

		if (gnutls_x509_crt_export
		    (cert, GNUTLS_X509_FMT_PEM, cert_buf, &len) >= 0)
			apr_table_setn(env,
				       apr_pstrcat(r->pool, MGS_SIDE,
						   "_CERT", NULL),
				       apr_pstrmemdup(r->pool, cert_buf,
						      len));

	}

	len = sizeof(buf);
	gnutls_x509_crt_get_dn(cert, buf, &len);
	apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_S_DN", NULL),
		       apr_pstrmemdup(r->pool, buf, len));

	len = sizeof(buf);
	gnutls_x509_crt_get_issuer_dn(cert, buf, &len);
	apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_I_DN", NULL),
		       apr_pstrmemdup(r->pool, buf, len));

	len = sizeof(sbuf);
	gnutls_x509_crt_get_serial(cert, sbuf, &len);
	tmp = mgs_session_id2sz(sbuf, len, buf, sizeof(buf));
	apr_table_setn(env,
		       apr_pstrcat(r->pool, MGS_SIDE, "_M_SERIAL", NULL),
		       apr_pstrdup(r->pool, tmp));

	ret = gnutls_x509_crt_get_version(cert);
	if (ret > 0)
		apr_table_setn(env,
			       apr_pstrcat(r->pool, MGS_SIDE, "_M_VERSION",
					   NULL), apr_psprintf(r->pool,
							       "%u", ret));

	apr_table_setn(env,
		       apr_pstrcat(r->pool, MGS_SIDE, "_CERT_TYPE", NULL),
		       "X.509");

	tmp =
	    mgs_time2sz(gnutls_x509_crt_get_expiration_time
			(cert), buf, sizeof(buf));
	apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_V_END", NULL),
		       apr_pstrdup(r->pool, tmp));

	tmp =
	    mgs_time2sz(gnutls_x509_crt_get_activation_time
			(cert), buf, sizeof(buf));
	apr_table_setn(env,
		       apr_pstrcat(r->pool, MGS_SIDE, "_V_START", NULL),
		       apr_pstrdup(r->pool, tmp));

	ret = gnutls_x509_crt_get_signature_algorithm(cert);
	if (ret >= 0) {
		apr_table_setn(env,
			       apr_pstrcat(r->pool, MGS_SIDE, "_A_SIG",
					   NULL),
			       gnutls_sign_algorithm_get_name(ret));
	}

	ret = gnutls_x509_crt_get_pk_algorithm(cert, NULL);
	if (ret >= 0) {
		apr_table_setn(env,
			       apr_pstrcat(r->pool, MGS_SIDE, "_A_KEY",
					   NULL),
			       gnutls_pk_algorithm_get_name(ret));
	}

	/* export all the alternative names (DNS, RFC822 and URI) */
	for (i = 0; !(ret < 0); i++) {
		len = 0;
		ret = gnutls_x509_crt_get_subject_alt_name(cert, i,
							   NULL, &len,
							   NULL);

		if (ret == GNUTLS_E_SHORT_MEMORY_BUFFER && len > 1) {
			tmp2 = apr_palloc(r->pool, len + 1);

			ret =
			    gnutls_x509_crt_get_subject_alt_name(cert, i,
								 tmp2,
								 &len,
								 NULL);
			tmp2[len] = 0;

			if (ret == GNUTLS_SAN_DNSNAME) {
				apr_table_setn(env,
					       apr_psprintf(r->pool,
							    "%s_S_AN%u",
							    MGS_SIDE, i),
					       apr_psprintf(r->pool,
							    "DNSNAME:%s",
							    tmp2));
			} else if (ret == GNUTLS_SAN_RFC822NAME) {
				apr_table_setn(env,
					       apr_psprintf(r->pool,
							    "%s_S_AN%u",
							    MGS_SIDE, i),
					       apr_psprintf(r->pool,
							    "RFC822NAME:%s",
							    tmp2));
			} else if (ret == GNUTLS_SAN_URI) {
				apr_table_setn(env,
					       apr_psprintf(r->pool,
							    "%s_S_AN%u",
							    MGS_SIDE, i),
					       apr_psprintf(r->pool,
							    "URI:%s",
							    tmp2));
			} else {
				apr_table_setn(env,
					       apr_psprintf(r->pool,
							    "%s_S_AN%u",
							    MGS_SIDE, i),
					       "UNSUPPORTED");
			}
		}
	}
}

static void
mgs_add_common_pgpcert_vars(request_rec * r, gnutls_openpgp_crt_t cert,
			    int side, int export_certificates_enabled)
{
	unsigned char sbuf[64];	/* buffer to hold serials */
	char buf[AP_IOBUFSIZE];
	const char *tmp;
	size_t len;
	int ret;

	if (r == NULL)
		return;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	apr_table_t *env = r->subprocess_env;

	if (export_certificates_enabled != 0) {
		char cert_buf[10 * 1024];
		len = sizeof(cert_buf);

		if (gnutls_openpgp_crt_export
		    (cert, GNUTLS_OPENPGP_FMT_BASE64, cert_buf, &len) >= 0)
			apr_table_setn(env,
				       apr_pstrcat(r->pool, MGS_SIDE,
						   "_CERT", NULL),
				       apr_pstrmemdup(r->pool, cert_buf,
						      len));

	}

	len = sizeof(buf);
	gnutls_openpgp_crt_get_name(cert, 0, buf, &len);
	apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_NAME", NULL),
		       apr_pstrmemdup(r->pool, buf, len));

	len = sizeof(sbuf);
	gnutls_openpgp_crt_get_fingerprint(cert, sbuf, &len);
	tmp = mgs_session_id2sz(sbuf, len, buf, sizeof(buf));
	apr_table_setn(env,
		       apr_pstrcat(r->pool, MGS_SIDE, "_FINGERPRINT",
				   NULL), apr_pstrdup(r->pool, tmp));

	ret = gnutls_openpgp_crt_get_version(cert);
	if (ret > 0)
		apr_table_setn(env,
			       apr_pstrcat(r->pool, MGS_SIDE, "_M_VERSION",
					   NULL), apr_psprintf(r->pool,
							       "%u", ret));

	apr_table_setn(env,
		       apr_pstrcat(r->pool, MGS_SIDE, "_CERT_TYPE", NULL),
		       "OPENPGP");

	tmp =
	    mgs_time2sz(gnutls_openpgp_crt_get_expiration_time
			(cert), buf, sizeof(buf));
	apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_V_END", NULL),
		       apr_pstrdup(r->pool, tmp));

	tmp =
	    mgs_time2sz(gnutls_openpgp_crt_get_creation_time
			(cert), buf, sizeof(buf));
	apr_table_setn(env,
		       apr_pstrcat(r->pool, MGS_SIDE, "_V_START", NULL),
		       apr_pstrdup(r->pool, tmp));

	ret = gnutls_openpgp_crt_get_pk_algorithm(cert, NULL);
	if (ret >= 0) {
		apr_table_setn(env,
			       apr_pstrcat(r->pool, MGS_SIDE, "_A_KEY",
					   NULL),
			       gnutls_pk_algorithm_get_name(ret));
	}

}

/* TODO: Allow client sending a X.509 certificate chain */
static int mgs_cert_verify(request_rec * r, mgs_handle_t * ctxt)
{
	const gnutls_datum_t *cert_list;
	unsigned int cert_list_size, status;
	int rv = GNUTLS_E_NO_CERTIFICATE_FOUND, ret;
	unsigned int ch_size = 0;
	union {
		gnutls_x509_crt_t x509[MAX_CHAIN_SIZE];
		gnutls_openpgp_crt_t pgp;
	} cert;
	apr_time_t expiration_time, cur_time;

	if (r == NULL || ctxt == NULL || ctxt->session == NULL)
		return HTTP_FORBIDDEN;

	_gnutls_log(debug_log_fp, "%s: %d\n", __func__, __LINE__);
	cert_list =
	    gnutls_certificate_get_peers(ctxt->session, &cert_list_size);

	if (cert_list == NULL || cert_list_size == 0) {
		/* It is perfectly OK for a client not to send a certificate if on REQUEST mode
		 */
		if (ctxt->sc->client_verify_mode == GNUTLS_CERT_REQUEST)
			return OK;

		/* no certificate provided by the client, but one was required. */
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Failed to Verify Peer: "
			      "Client did not submit a certificate");
		return HTTP_FORBIDDEN;
	}

	if (gnutls_certificate_type_get(ctxt->session) == GNUTLS_CRT_X509) {
		ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
			      "GnuTLS: A Chain of %d certificate(s) was provided for validation",
			      cert_list_size);

		for (ch_size = 0; ch_size < cert_list_size; ch_size++) {
			gnutls_x509_crt_init(&cert.x509[ch_size]);
			rv = gnutls_x509_crt_import(cert.x509[ch_size],
						    &cert_list[ch_size],
						    GNUTLS_X509_FMT_DER);
			// When failure to import, leave the loop
			if (rv != GNUTLS_E_SUCCESS) {
				if (ch_size < 1) {
					ap_log_rerror(APLOG_MARK,
						      APLOG_INFO, 0, r,
						      "GnuTLS: Failed to Verify Peer: "
						      "Failed to import peer certificates.");
					ret = HTTP_FORBIDDEN;
					goto exit;
				}
				ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
					      "GnuTLS: Failed to import some peer certificates. Using %d certificates",
					      ch_size);
				rv = GNUTLS_E_SUCCESS;
				break;
			}
		}
	} else if (gnutls_certificate_type_get(ctxt->session) ==
		   GNUTLS_CRT_OPENPGP) {
		if (cert_list_size > 1) {
			ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
				      "GnuTLS: Failed to Verify Peer: "
				      "Chained Client Certificates are not supported.");
			return HTTP_FORBIDDEN;
		}

		gnutls_openpgp_crt_init(&cert.pgp);
		rv = gnutls_openpgp_crt_import(cert.pgp, &cert_list[0],
					       GNUTLS_OPENPGP_FMT_RAW);

	} else
		return HTTP_FORBIDDEN;

	if (rv < 0) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Failed to Verify Peer: "
			      "Failed to import peer certificates.");
		ret = HTTP_FORBIDDEN;
		goto exit;
	}

	if (gnutls_certificate_type_get(ctxt->session) == GNUTLS_CRT_X509) {
		apr_time_ansi_put(&expiration_time,
				  gnutls_x509_crt_get_expiration_time
				  (cert.x509[0]));

		ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
			      "GnuTLS: Verifying list of  %d certificate(s)",
			      ch_size);
		rv = gnutls_x509_crt_list_verify(cert.x509, ch_size,
						 ctxt->sc->ca_list,
						 ctxt->sc->ca_list_size,
						 NULL, 0, 0, &status);
	} else {
		apr_time_ansi_put(&expiration_time,
				  gnutls_openpgp_crt_get_expiration_time
				  (cert.pgp));

		rv = gnutls_openpgp_crt_verify_ring(cert.pgp,
						    ctxt->sc->pgp_list, 0,
						    &status);
	}

	if (rv < 0) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Failed to Verify Peer certificate: (%d) %s",
			      rv, gnutls_strerror(rv));
		if (rv == GNUTLS_E_NO_CERTIFICATE_FOUND)
			ap_log_rerror(APLOG_MARK, APLOG_EMERG, 0, r,
				      "GnuTLS: No certificate was found for verification. Did you set the GnuTLSX509CAFile or GnuTLSPGPKeyringFile directives?");
		ret = HTTP_FORBIDDEN;
		goto exit;
	}

	/* TODO: X509 CRL Verification. */
	/* May add later if anyone needs it.
	 */
	/* ret = gnutls_x509_crt_check_revocation(crt, crl_list, crl_list_size); */

	cur_time = apr_time_now();

	if (status & GNUTLS_CERT_SIGNER_NOT_FOUND) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Could not find Signer for Peer Certificate");
	}

	if (status & GNUTLS_CERT_SIGNER_NOT_CA) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Peer's Certificate signer is not a CA");
	}

	if (status & GNUTLS_CERT_INSECURE_ALGORITHM) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Peer's Certificate is using insecure algorithms");
	}

	if (status & GNUTLS_CERT_EXPIRED
	    || status & GNUTLS_CERT_NOT_ACTIVATED) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Peer's Certificate signer is expired or not yet activated");
	}

	if (status & GNUTLS_CERT_INVALID) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Peer Certificate is invalid.");
	} else if (status & GNUTLS_CERT_REVOKED) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			      "GnuTLS: Peer Certificate is revoked.");
	}

	if (gnutls_certificate_type_get(ctxt->session) == GNUTLS_CRT_X509)
		mgs_add_common_cert_vars(r, cert.x509[0], 1,
					 ctxt->
					 sc->export_certificates_enabled);
	else if (gnutls_certificate_type_get(ctxt->session) ==
		 GNUTLS_CRT_OPENPGP)
		mgs_add_common_pgpcert_vars(r, cert.pgp, 1,
					    ctxt->
					    sc->export_certificates_enabled);

	{
		/* days remaining */
		unsigned long remain =
		    (apr_time_sec(expiration_time) -
		     apr_time_sec(cur_time)) / 86400;
		apr_table_setn(r->subprocess_env, "SSL_CLIENT_V_REMAIN",
			       apr_psprintf(r->pool, "%lu", remain));
	}

	if (status == 0) {
		apr_table_setn(r->subprocess_env, "SSL_CLIENT_VERIFY",
			       "SUCCESS");
		ret = OK;
	} else {
		apr_table_setn(r->subprocess_env, "SSL_CLIENT_VERIFY",
			       "FAILED");
		if (ctxt->sc->client_verify_mode == GNUTLS_CERT_REQUEST)
			ret = OK;
		else
			ret = HTTP_FORBIDDEN;
	}

      exit:
	if (gnutls_certificate_type_get(ctxt->session) == GNUTLS_CRT_X509) {
		int i;
		for (i = 0; i < ch_size; i++) {
			gnutls_x509_crt_deinit(cert.x509[i]);
		}
	} else if (gnutls_certificate_type_get(ctxt->session) ==
		   GNUTLS_CRT_OPENPGP)
		gnutls_openpgp_crt_deinit(cert.pgp);
	return ret;


}

void mgs_hook_opt_retr(void) {
#ifdef ENABLE_SRP
	if (mgs_dbd_prepare_fn == NULL) {
		mgs_dbd_prepare_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_prepare);
		mgs_dbd_open_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_open);
		mgs_dbd_close_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_close);
	}
#endif
}

#ifdef ENABLE_SRP
static int mgs_srp_server_credentials(gnutls_session_t session,
				      const char * username,
				      gnutls_datum_t * salt,
				      gnutls_datum_t * verifier,
				      gnutls_datum_t * g, gnutls_datum_t * n)
{
	apr_status_t rv;
	mgs_handle_t *ctxt = gnutls_transport_get_ptr(session);
	apr_pool_t *pool = ctxt->c->pool;
	mgs_srvconf_rec *sc = ctxt->sc;
	server_rec *server = sc->server;
	ap_dbd_t *dbd;
	apr_dbd_row_t *row = NULL;
	apr_dbd_prepared_t *statement;
	apr_dbd_results_t *res = NULL;
	int i;
	const char *col, *val;

	/* Get database handle. */
	if (mgs_dbd_open_fn == NULL) {
		ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, ctxt->c,
			      "Failed to get mod_dbd open function pointer "
			      "-- must enable mod_dbd");
		return -1;
	}
	dbd = mgs_dbd_open_fn(pool, server);
	if (dbd == NULL) {
		ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, ctxt->c,
			      "Failed to get SRP passwd DB handle");
		return -1;
	}

	/* Execute query. */
	if (ctxt->sc->srp_passwd_query == NULL) {
		ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, ctxt->c,
			     "GnuTLSSRPPasswdQuery wasn't set");
		mgs_dbd_close_fn(server, dbd);
		return -1;
	}
	statement = apr_hash_get(dbd->prepared, sc->srp_passwd_query,
				 APR_HASH_KEY_STRING);
	if (statement == NULL) {
		ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, ctxt->c,
			      "A prepared statement could not be found for "
			      "GnuTLSSRPPasswdQuery with the key '%s' on "
			      "server %s",
			      sc->srp_passwd_query, sc->server->defn_name);
		mgs_dbd_close_fn(server, dbd);
		return -1;
	}
	if (apr_dbd_pvselect(dbd->driver, pool, dbd->handle, &res,
			     statement, 0, username, NULL) != 0) {
		ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, ctxt->c,
			      "Query execution error looking up '%s' "
			      "in SRP passwd DB", username);
		mgs_dbd_close_fn(server, dbd);
		return -1;
	}

	/* Clear outputs. */
	n->size = 0;	 n->data = NULL;
	g->size = 0;	 g->data = NULL;
	verifier->size = 0;  verifier->data = NULL;
	salt->size = 0;      salt->data = NULL;

	/* Get query results. */
	rv = apr_dbd_get_row(dbd->driver, pool, res, &row, -1);

	/* Iterate through result columns if found user. */
	if (rv == 0) {
		i = 0;
		while ((col = apr_dbd_get_name(dbd->driver, res, i)) != NULL) {
			gnutls_datum_t *item = NULL;
			val = apr_dbd_get_entry(dbd->driver, row, i);
			i++;
                        if (val == NULL)
                                continue;
			if (strcmp(col, "srp_group") == 0) {
				if (strcmp(val, "1024") == 0) {
					*n = gnutls_srp_1024_group_prime;
					*g = gnutls_srp_1024_group_generator;
				} else if (strcmp(val, "1536") == 0) {
					*n = gnutls_srp_1536_group_prime;
					*g = gnutls_srp_1536_group_generator;
				} else if (strcmp(val, "2048") == 0) {
					*n = gnutls_srp_2048_group_prime;
					*g = gnutls_srp_2048_group_generator;
				} else {
					ap_log_cerror(APLOG_MARK, APLOG_ERR,
						      0, ctxt->c,
						      "Unknown SRP group: %s",
						      val);
				}
			} else if (strcmp(col, "srp_v") == 0) {
				item = verifier;
			} else if (strcmp(col, "srp_s") == 0) {
				item = salt;
			}
			if (item != NULL) {
				item->data = gnutls_malloc
					(apr_base64_decode_len(val));
				if (item->data) {
					item->size = apr_base64_decode
						((char *)item->data, val);
				} else {
					ap_log_cerror(APLOG_MARK, APLOG_CRIT,
						      0, ctxt->c,
						      "gnutls_malloc failed");
				}
			}
		}
	}

	mgs_dbd_close_fn(server, dbd);

	/* Use random params if user isn't found. */
	if (!(g->size && n->size && salt->size && verifier->size)) {
		ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, ctxt->c,
			      "SRP user '%s' not found, using random params",
			      username);
		if (verifier->data)
			gnutls_free(verifier->data);
		if (salt->data)
			gnutls_free(salt->data);
		*n = gnutls_srp_1024_group_prime;
		*g = gnutls_srp_1024_group_generator;
		return 1; /* 1 means use random params */
	}

	return 0;
}

#endif
