/* --------------------------------------------------------------------------
 *
 * License
 *
 * The contents of this file are subject to the Jabber Open Source License
 * Version 1.0 (the "License").  You may not copy or use this file, in either
 * source code or executable form, except in compliance with the License.  You
 * may obtain a copy of the License at http://www.jabber.com/license/ or at
 * http://www.opensource.org/.  
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied.  See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * Copyrights
 * 
 * Portions created by or assigned to Jabber.com, Inc. are 
 * Copyright (c) 1999-2000 Jabber.com, Inc.  All Rights Reserved.  Contact
 * information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 * Portions Copyright (c) 1998-1999 Jeremie Miller.
 * 
 * Acknowledgements
 * 
 * Special thanks to the Jabber Open Source Contributors for their
 * suggestions and support of Jabber.
 * 
 * --------------------------------------------------------------------------*/
#include "jsm.h"


/* takes any packet and attempts to deliver it to the correct session/thread */
/* must have a valid to/from address already before getting here */
void js_deliver_local(jsmi si, jpacket p, HASHTABLE ht)
{
    udata user = NULL;
    session s = NULL;

    /* first, collect some facts */
    user = js_user(si, p->to, ht);
    s = js_session_get(user, p->to->resource);

    log_debug(ZONE,"delivering locally to %s",jid_full(p->to));
    /* let some modules fight over it */
    if(js_mapi_call(si, e_DELIVER, p, user, s))
        return;

    if(p->to->user == NULL)
    { /* this is for the server */
        js_psend(si->mpserver,p);
        return;
    }

    if(s != NULL)
    { /* it's sent right to the resource */
        js_session_to(s, p);
        return;
    }

    if(user != NULL)
    { /* valid user, but no session */
        p->aux1 = (void *)user; /* performance hack, we already know the user */
        user->ref++; /* so it doesn't get cleaned up before the thread gets it */
        js_psend(si->mpoffline,p);
        return;
    }

    /* no user, so bounce the packet */
    js_bounce(si,p->x,TERROR_NOTFOUND);
}


result js_packet(instance i, dpacket p, void *arg)
{
    jsmi si = (jsmi)arg;
    jpacket jp = NULL;
    HASHTABLE ht;
    session s = NULL;
    udata u;
    char *type, *authto;

    log_debug(ZONE,"(%X)incoming packet %s",si,xmlnode2str(p->x));

    /* make sure this hostname is in the master table */
    if((ht = (HASHTABLE)ghash_get(si->hosts,p->host)) == NULL)
    {
        ht = ghash_create(j_atoi(xmlnode_get_data(js_config(si,"maxusers")),USERS_PRIME),(KEYHASHFUNC)str_hash_code,(KEYCOMPAREFUNC)j_strcmp);
        log_debug(ZONE,"creating user hash %X for %s",ht,p->host);
        ghash_put(si->hosts,pstrdup(si->p,p->host), (void *)ht);
        log_debug(ZONE,"checking %X",ghash_get(si->hosts,p->host));
    }

    /* if this is a routed packet */
    if(p->type == p_ROUTE)
    {
        type = xmlnode_get_attrib(p->x,"type");

        /* new session requests */
        if(j_strcmp(type,"session") == 0)
        {
            if((s = js_session_new(si, p)) == NULL)
            {
                /* session start failed */
                log_warn(p->host,"Unable to create session %s",jid_full(p->id));
                xmlnode_put_attrib(p->x,"type","error");
                xmlnode_put_attrib(p->x,"error","Session Failed");
            }else{
                /* reset to the routed id for this session for the reply below */
                xmlnode_put_attrib(p->x,"to",jid_full(s->route));
            }

            /* reply */
            jutil_tofrom(p->x);
            deliver(dpacket_new(p->x), i);
            return r_DONE;
        }

        /* get the internal jpacket */
        if(xmlnode_get_firstchild(p->x) != NULL) /* XXX old libjabber jpacket_new() wasn't null safe, this is just safety */
            jp = jpacket_new(xmlnode_get_firstchild(p->x));

        /* auth/reg requests */
        if(jp != NULL && j_strcmp(type,"auth") == 0)
        {
            /* check and see if we're configured to forward auth packets for processing elsewhere */
            if((authto = xmlnode_get_data(js_config(si,"auth"))) != NULL)
            {
                xmlnode_put_attrib(p->x,"oto",xmlnode_get_attrib(p->x,"to")); /* preserve original to */
                xmlnode_put_attrib(p->x,"to",authto);
                deliver(dpacket_new(p->x), i);
                return r_DONE;
            }

            /* internally, hide the route to/from addresses on the authreg request */
            xmlnode_put_attrib(jp->x,"to",xmlnode_get_attrib(p->x,"to"));
            xmlnode_put_attrib(jp->x,"from",xmlnode_get_attrib(p->x,"from"));
            xmlnode_put_attrib(jp->x,"route",xmlnode_get_attrib(p->x,"type"));
            jpacket_reset(jp);
            jp->aux1 = (void *)si;
            mtq_send(NULL,jp->p,js_authreg,(void *)jp);
            return r_DONE;
        }

        /* this is a packet to be processed as outgoing for a session */

        /* attempt to locate the session by matching the special resource */
        u = js_user(si, p->id, ht);
        if(u == NULL)
        {
            /* no user!?!?! */
            log_notice(p->host,"Bouncing packet intended for nonexistant user: %s",xmlnode2str(p->x));
            deliver_fail(dpacket_new(p->x), "Invalid User");
            return r_DONE;
        }

        for(s = u->sessions; s != NULL; s = s->next)
            if(j_strcmp(p->id->resource, s->route->resource) == 0)
                break;

        /* if it's an error */
        if(j_strcmp(type,"error") == 0)
        {
            /* ooh, incoming routed errors in reference to this session, the session is kaput */
            if(s != NULL)
            {
                s->sid = NULL; /* they generated the error, no use in sending there anymore! */
                js_session_end(s, "Disconnected");
            }

            /* if this was a message, it should have been delievered to that session, store offline */
            if(jp != NULL && jp->type == JPACKET_MESSAGE)
            {
                js_deliver_local(si, jp, ht); /* (re)deliver it locally again, should go to another session or offline */
                return r_DONE;
            }
            /* drop and return */
            if(xmlnode_get_firstchild(p->x) != NULL)
                log_notice(p->host, "Dropping a bounced session packet to %s", jid_full(p->id));
            xmlnode_free(p->x);
            return r_DONE;
        }

        if(jp == NULL)
        { /* uhh, empty packet, *shrug* */
            log_notice(p->host,"Dropping an invalid or empty route packet: %s",xmlnode2str(p->x),jid_full(p->id));
            xmlnode_free(p->x);
            return r_DONE;
        }

        if(s != NULL)
        {   /* just pass to the session normally */
            js_session_from(s, jp);
        }else{
            /* bounce back as an error */
            log_notice(p->host,"Bouncing %s packet intended for session %s",xmlnode_get_name(jp->x),jid_full(p->id));
            deliver_fail(dpacket_new(p->x), "Invalid Session");
        }
        return r_DONE;
    }

    /* normal server-server packet, should we make sure it's not spoofing us?  if so, if ghash_get(p->to->server) then bounce w/ security error */

    jp = jpacket_new(p->x);
    if(jp == NULL)
    {
        log_warn(p->host,"Dropping invalid incoming packet: %s",xmlnode2str(p->x));
        xmlnode_free(p->x);
        return r_DONE;
    }

    js_deliver_local(si, jp, ht);

    return r_DONE;
}


/* NOTE: any jpacket sent to deliver *MUST* match jpacket_new(p->x),
 * jpacket is simply a convenience wrapper
 */
void js_deliver(jsmi si, jpacket p)
{
    HASHTABLE ht;

    if(p->to == NULL)
    {
        log_warn(NULL,"jsm: Invalid Recipient, returning data %s",xmlnode2str(p->x));
        js_bounce(si,p->x,TERROR_BAD);
        return;
    }

    if(p->from == NULL)
    {
        log_warn(NULL,"jsm: Invalid Sender, discarding data %s",xmlnode2str(p->x));
        xmlnode_free(p->x);
        return;
    }

    log_debug(ZONE,"deliver(to[%s],from[%s],type[%d],packet[%s])",jid_full(p->to),jid_full(p->from),p->type,xmlnode2str(p->x));

    /* external to us */
    if((ht = (HASHTABLE)ghash_get(si->hosts,p->to->server)) != NULL)
    {
        js_deliver_local(si, p, ht);
        return;
    }
    deliver(dpacket_new(p->x), si->i);

}


void js_psend(pth_msgport_t mp, jpacket p)
{
    jpq q;

    if(p == NULL || mp == NULL)
        return;

    log_debug(ZONE,"psending to %X packet %X",mp,p);

    q = pmalloc(p->p, sizeof(_jpq));
    q->p = p;

    pth_msgport_put(mp, (pth_message_t *)q);
}


/* for fun, a tidbit from late nite irc (ya had to be there)
<temas> What is 1+1
<temas> Why did you hardcode stuff
<temas> Was the movie good?
<temas> DId the nukes explode?
*/
