/* --------------------------------------------------------------------------
 *
 * License
 *
 * The contents of this file are subject to the Jabber Open Source License
 * Version 1.0 (the "JOSL").  You may not copy or use this file, in either
 * source code or executable form, except in compliance with the JOSL. You
 * may obtain a copy of the JOSL at http://www.jabber.org/ or at
 * http://www.opensource.org/.  
 *
 * Software distributed under the JOSL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied.  See the JOSL
 * for the specific language governing rights and limitations under the
 * JOSL.
 *
 * Copyrights
 * 
 * Portions created by or assigned to Jabber.com, Inc. are 
 * Copyright (c) 1999-2002 Jabber.com, Inc.  All Rights Reserved.  Contact
 * information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 * Portions Copyright (c) 1998-1999 Jeremie Miller.
 * 
 * Acknowledgements
 * 
 * Special thanks to the Jabber Open Source Contributors for their
 * suggestions and support of Jabber.
 * 
 * Alternatively, the contents of this file may be used under the terms of the
 * GNU General Public License Version 2 or later (the "GPL"), in which case
 * the provisions of the GPL are applicable instead of those above.  If you
 * wish to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the JOSL,
 * indicate your decision by deleting the provisions above and replace them
 * with the notice and other provisions required by the GPL.  If you do not
 * delete the provisions above, a recipient may use your version of this file
 * under either the JOSL or the GPL. 
 * 
 * --------------------------------------------------------------------------*/

#include "jadc2s.h"

/* check jadc2s.h for an overview of this codebase */
static int process_conns = 1;

/* this checks for bad conns that have timed out */
void _walk_pending(xht pending, const char *key, void *val, void *arg)
{
    conn_t c = (conn_t)val;
    time_t now = (time_t)arg;

    /* send stream error and close connection */
    if((now - c->start) > c->c2s->timeout)
	conn_close(c, STREAM_ERR_TIMEOUT, "You have not authenticated in time");
}

/***
* Iterate over the bad conns list and reset people that are ok
* @param c2s The c2s instance to process from
*/
void check_karma(c2s_t c2s)
{
    bad_conn_t cur, next;
    time_t start;

    time(&start);

    cur = c2s->bad_conns;
    while ( (cur != NULL) && (cur->last < start) )
    {
        next = cur->next;
        /* Let them read again */
        mio_read(c2s->mio, cur->c->fd);
        /* cleanup and move on in the list */
        free(cur);

        cur = next;
    }

    /* update the pointer to the first bad connection */
    c2s->bad_conns = cur;
    if (c2s->bad_conns == NULL)
    {
	/* XXX Make this a config option? */
	c2s->timeout = 15;
    }
}

static void usage(void)
{
    fputs(
	"This is version " VERSION " of jadc2s\n"
#ifdef USE_SSL
	"SSL/TLS is enabled in this build.\n"
#endif
	"\n"
        "Usage: jadc2s <options>\n"
        "Options are:\n"
	"   -h              output this help message\n"
        "   -c <config>     config file to use\n"
	"                   [default: " CONFIG_DIR "/jadc2s.xml]\n"
	"   -r <randdev>    device to read randomization seed from\n"
        "   -o key=value    override config file property\n"
        "   -b              run in background\n", stdout);
}

/* 
 * Handle signals that would stop us nicely
 */
void onSignal(int signal)
{
    /* Just tell ourselves to stop process */
    process_conns = 0;
}

/* although this is our main and it's an all-in-one right now,
 * it's done in a way that would make it quite easy to thread, 
 * customize, or integrate with another codebase
 */
int main(int argc, char **argv)
{
    c2s_t c2s;
    time_t last_log, last_pending, last_jid_clean, now;
    char optchar;
    int config_loaded = 0;
    int i, fd;
    char *rand_dev = NULL;
    unsigned int rand_seed = 0;

    signal(SIGINT, onSignal);
    signal(SIGPIPE, SIG_IGN);
    
    /* set up our c2s global stuff */
    c2s = (c2s_t)malloc(sizeof(struct c2s_st));
    memset(c2s, 0, sizeof(struct c2s_st));

    /* create environment for jid preparation */
    c2s->jid_environment = jid_new_environment();

    /* set default for rand_dev */
    rand_dev = strdup("/dev/urandom");

    /* load our config */
    c2s->config = config_new();

    /* cmdline parsing */
    while((optchar = getopt(argc, argv, "c:r:bh?")) >= 0)
    {
        switch(optchar)
        {
            case 'h': case '?':
                usage();
                return 1;
	    case 'r':
		free(rand_dev);
		rand_dev = strdup(optarg);
		break;
            case 'c':
                if(!config_load(c2s->config, optarg))
                    config_loaded++;
                break;
    	    case 'b':
                /* !!! needs testing */
                /* !!! should this be a cmdline option, or in the config? */
                daemonize();
                break;	       
        }
    }

    if(!config_loaded && config_load(c2s->config, CONFIG_DIR "/jadc2s.xml"))
    {
        fprintf(stderr, "no config loaded, aborting\n");
        usage();
        return 1;
    }

    /* inbuilt defaults and config file options */
    /* !!! config options for these? */
    c2s->connection_rates = xhash_new(199);
    c2s->pending = xhash_new(199);
    c2s->bad_conns = NULL;
    c2s->timeout = 15;

    /* conn setup */
    c2s->max_fds = j_atoi(config_get_one(c2s->config, "io.max_fds", 0), 1023);
    c2s->mio = mio_new(c2s->max_fds);
    c2s->conns = (struct conn_st *) malloc(sizeof(struct conn_st) * c2s->max_fds);
    memset(c2s->conns, 0, sizeof(struct conn_st) * c2s->max_fds);
    for(i = 0; i < c2s->max_fds; i++)
        c2s->conns[i].fd = -1;      /* -1 == unused */

    /* nad cache */
    c2s->nads = nad_cache_new();

    /* session manager */
    c2s->sm_host = config_get_one(c2s->config, "sm.host", 0);
    c2s->sm_port = j_atoi(config_get_one(c2s->config, "sm.port", 0), 0);
    c2s->sm_id = config_get_one(c2s->config, "sm.id", 0);
    c2s->sm_secret = config_get_one(c2s->config, "sm.secret", 0);

    c2s->connection_rate_times =
        j_atoi(config_get_one(c2s->config, "io.connection_limits.connects", 0), 0);
    c2s->connection_rate_seconds =
        j_atoi(config_get_one(c2s->config, "io.connection_limits.seconds", 0), 0);
    
    /* XXX Change before release */
    c2s->local_id = config_get(c2s->config,"local.id");
    c2s->local_alias = config_get(c2s->config,"local.alias");
    c2s->local_ip = config_get_one(c2s->config, "local.ip", 0);
    c2s->local_port = j_atoi(config_get_one(c2s->config, "local.port", 0), 5222);
#ifdef USE_SSL
    c2s->local_sslport = j_atoi(config_get_one(c2s->config, "local.ssl.port", 0), 5223);
    c2s->pemfile = config_get_one(c2s->config, "local.ssl.pemfile", 0);
    c2s->ciphers = config_get_one(c2s->config, "local.ssl.ciphers", 0);
    
    c2s->ssl_enable_workarounds = (config_get_one(c2s->config, "local.ssl.enable_workarounds", 0) != NULL);
    c2s->ssl_no_ssl_v2 = (config_get_one(c2s->config, "local.ssl.no_ssl_v2", 0) != NULL);
    c2s->ssl_no_ssl_v3 = (config_get_one(c2s->config, "local.ssl.no_ssl_v3", 0) != NULL);
    c2s->ssl_no_tls_v1 = (config_get_one(c2s->config, "local.ssl.no_tls_v1", 0) != NULL);
    c2s->ssl_enable_autodetect = (config_get_one(c2s->config, "local.ssl.enable_autodetect", 0) != NULL);
#endif
    c2s->iplog = (config_get_one(c2s->config, "io.iplog", 0) != NULL);

    /* require some things */
    if(c2s->sm_host == NULL) {
	fprintf(stderr, "Need the hostname where to find the router in the configuration\n");
	usage();		/* XXX usage isn't really helpful for this */
	return 1;
    }
    if(c2s->sm_port == 0) {
	fprintf(stderr, "Need the port number of the router in the configuration\n");
	usage();		/* XXX usage isn't really helpful for this */
	return 1;
    }
    if(c2s->sm_id == NULL) {
	fprintf(stderr, "Need our ID on the router in the configuration\n");
	usage();		/* XXX usage isn't really helpful for this */
	return 1;
    }
    if(c2s->sm_secret == NULL) {
	fprintf(stderr, "Need the secret for the router in the configuration\n");
	usage();		/* XXX usage isn't really helpful for this */
	return 1;
    }
    if(c2s->local_id == NULL) {
	fprintf(stderr, "Need our domain(s) for which connections should be accepted in the config\n");
	usage();		/* XXX usage isn't really helpful for this */
	return 1;
    }

    /* start logging */
    c2s->log = log_new(c2s->sm_id);
    log_write(c2s->log, LOG_NOTICE, "starting up");

    /* seed the random number generator */
    fd = open(rand_dev, O_RDONLY|O_NOCTTY);
    if (fd != -1)
    {
	read(fd, &rand_seed, sizeof(rand_seed));
	close(fd);
    }
    if (rand_seed == 0)
    {
	log_write(c2s->log, LOG_NOTICE, "could not seed random number generator from %s - using time", rand_dev);
	rand_seed = time(NULL);
    }
    srand(rand_seed);
    free(rand_dev);

    /* first, make sure we can connect to our sm */
    if (!connect_new(c2s))
    {
        log_write(c2s->log, LOG_ERR, "Unable to connect to sm!");
        exit(1);
    }

    /* only bind the unencrypted port if we have a real port number for it */
    if(c2s->local_port > 0)
    {
        /* then make sure we can listen */
        if(mio_listen(c2s->mio, c2s->local_port, c2s->local_ip, client_io, (void*)c2s) < 0)
        {
            log_write(c2s->log, LOG_ERR, "failed to listen on port %d!", c2s->local_port);
            return 1;
        }

        log_write(c2s->log, LOG_NOTICE, "listening for client connections on port %d", c2s->local_port);
    }

#ifdef USE_SSL
    /* get the SSL port all set up */
    if(c2s->local_sslport == 0 || c2s->pemfile == NULL)
        log_write(c2s->log, LOG_WARNING, "ssl port or pem file not specified, ssl disabled");
    else
    {
        /* init the OpenSSL library */
        OpenSSL_add_ssl_algorithms();
        SSL_load_error_strings();
        c2s->ssl_ctx = SSL_CTX_new(SSLv23_server_method());

	if (c2s->ssl_ctx == NULL)
	{
	    log_write(c2s->log, LOG_ERR, "failed to initialize SSL/TLS library");
	    log_ssl_errors(c2s->log, LOG_ERR);
	    return 1;
	}

        /* if these fail, we keep the context hanging around, because we free it at shutdown */
        if(SSL_CTX_use_certificate_file(c2s->ssl_ctx, c2s->pemfile, SSL_FILETYPE_PEM) != 1)
	{
            log_write(c2s->log, LOG_WARNING, "failed to load certificate from %s, ssl disabled", c2s->pemfile);
	    log_ssl_errors(c2s->log, LOG_WARNING);
	}
        else if(SSL_CTX_use_PrivateKey_file(c2s->ssl_ctx, c2s->pemfile, SSL_FILETYPE_PEM) != 1)
	{
            log_write(c2s->log, LOG_WARNING, "failed to load private key from %s, ssl disabled", c2s->pemfile);
	    log_ssl_errors(c2s->log, LOG_WARNING);
	}
        else
        {
            if(!SSL_CTX_check_private_key(c2s->ssl_ctx))
	    {
                log_write(c2s->log, LOG_WARNING, "private key does not match certificate public key, ssl disabled");
		log_ssl_errors(c2s->log, LOG_WARNING);
	    }
	    else
	    {
		if (c2s->ciphers != NULL && !SSL_CTX_set_cipher_list(c2s->ssl_ctx, c2s->ciphers))
		{
		    log_write(c2s->log, LOG_ERR, "non of the configured ciphers could be enabled, ssl disabled");
		    log_ssl_errors(c2s->log, LOG_ERR);
		}
		if(mio_listen(c2s->mio, c2s->local_sslport, c2s->local_ip, client_io, (void*)c2s) < 0)
		    log_write(c2s->log, LOG_ERR, "failed to listen on port %d!", c2s->local_sslport);
		else
		    log_write(c2s->log, LOG_NOTICE, "listening for ssl client connections on port %d", c2s->local_sslport);
	    }
        }

	/* enable workarounds for different SSL client bugs or disable
	 * some versions of SSL/TLS */
	if (c2s->ssl_enable_workarounds)
	    SSL_CTX_set_options(c2s->ssl_ctx, SSL_OP_ALL);
	if (c2s->ssl_no_ssl_v2)
	    SSL_CTX_set_options(c2s->ssl_ctx, SSL_OP_NO_SSLv2);
	if (c2s->ssl_no_ssl_v3)
	    SSL_CTX_set_options(c2s->ssl_ctx, SSL_OP_NO_SSLv3);
	if (c2s->ssl_no_tls_v1)
	    SSL_CTX_set_options(c2s->ssl_ctx, SSL_OP_NO_TLSv1);
    }
#endif

    /* just a matter of processing socket events now */
    last_jid_clean = last_pending = last_log = time(NULL);
    while(process_conns)
    {
        mio_run(c2s->mio, c2s->timeout);

        /* log this no more than once per minute */
        if((time(NULL) - last_log) > 60)
        {
            log_write(c2s->log, LOG_NOTICE, "current number of clients: %d",c2s->num_clients);
            last_log = time(NULL);
        }

        /* !!! XXX Should these be configurable cleanup times? */
        /* every so often check for timed out pending conns */
        if((time(&now) - last_pending) > 15)
        {
            xhash_walk(c2s->pending, _walk_pending, (void*)now);
            last_pending = time(NULL);
        }

	/* cleanup the stringprep caches */
	if ((time(NULL) - last_jid_clean) > 60) {
	    jid_clean_cache(c2s->jid_environment);
	}

        /* !!! XXX Move this in here for optimization? */
        connection_rate_cleanup(c2s);

        /* XXX This still feels odd having more stuff in here */
        check_karma(c2s);
    }

    /* TODO: Notify sessionmanager about shutdown */
    log_write(c2s->log, LOG_NOTICE, "shutting down");

    /* close client connections */
    for(i = 0; i < c2s->max_fds; i++)
	if (c2s->conns[i].fd != -1 && c2s->conns[i].fd != c2s->sm->fd) 
	    conn_close(&c2s->conns[i], STREAM_ERR_SYSTEM_SHUTDOWN, "shutting down jadc2s");

    /* close session manager connection */
    c2s->shutting_down = 1;
    conn_close(c2s->sm, NULL, NULL);

    /* exiting, clean up */
    mio_free(c2s->mio);
    xhash_free(c2s->connection_rates);
    xhash_free(c2s->pending);
    free(c2s->conns);
    nad_cache_free(c2s->nads);
    log_free(c2s->log);
#ifdef USE_SSL
    SSL_CTX_free(c2s->ssl_ctx);
#endif
    xhash_free(c2s->config);
    free(c2s);

    pool_stat(1);

    return 0;
}

/* spit out debug output */
void debug_log(char *file, int line, const char *msgfmt, ...)
{
    va_list ap;
    char *pos, message[MAX_DEBUG];
    int sz;
    time_t t;

    /* timestamp */
    t = time(NULL);
    pos = ctime(&t);
    sz = strlen(pos);
    /* chop off the \n */
    pos[sz-1]=' ';

    /* insert the header */
    snprintf(message, MAX_DEBUG, "%s%s:%d ", pos, file, line);

    /* find the end and attach the rest of the msg */
    for (pos = message; *pos != '\0'; pos++); //empty statement
    sz = pos - message;
    va_start(ap, msgfmt);
    vsnprintf(pos, MAX_DEBUG - sz, msgfmt, ap);
    fprintf(stderr,"%s", message);
    fprintf(stderr, "\n");
}


int daemonize(void)
{
    pid_t pid;
    int i;

    fprintf(stderr, "preparing to run in daemon mode ...\n");
    
    if ((pid = fork()) != 0)
       exit(0);

    /* become session leader */
    setsid();

    /* ignore the signals we want */
    ignore_term_signals();
    
    if ((pid = fork()) != 0)
       exit(0);

    /* !!! could change to our working directory */
    // chdir("/"); 

    umask(0);

    /* close open descriptors */
    for (i=0; i < MAXFD; ++i)
       close(i);

    return 0;
}

int ignore_term_signals(void)
{
    /* !!! the list of signals to ignore -- needs to be reviewed */
    int sig[] = {SIGHUP, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU};

    int num_sigs = (sizeof sig) / (sizeof sig[0]);
    int i;
    struct sigaction dae_action;
    
    dae_action.sa_handler = SIG_IGN;
    sigemptyset(&(dae_action.sa_mask));
    dae_action.sa_flags = 0;
    
    for (i = 0; i < num_sigs; ++i)
    	if (sigaction(sig[i], &dae_action, NULL) == -1) return -1;
    
    return 0;
}
