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
 * server.c - thread that handles messages/packets intended for the server:
 *            administration, public IQ (agents, etc)
 * --------------------------------------------------------------------------*/

#include "jsm.h"

void *js_server_main(void *arg)
{
    jsmi si = (jsmi)arg;
    pth_event_t ev;		/* event ring for receiving messages */
    pth_msgport_t mp;	/* message port for receiving messages */
    jpq q;				/* a jabber packet */
    udata u = NULL;

    /* debug message */
    log_debug(ZONE,"THREAD:SERVER starting");

    /* create the message port and event ring */
    si->mpserver = mp = pth_msgport_create("js_server");
    ev = pth_event(PTH_EVENT_MSG,mp);

    /* infinite loop */
    while(1)
    {
        /* wait for a message */
        pth_wait(ev);

        /* get a packet from the message port */
        while((q = (jpq)pth_msgport_get(mp)) != NULL)
        {
            /* debug message */
            log_debug(ZONE,"THREAD:SERVER received a packet: %s",xmlnode2str(q->p->x));

            /* get the user struct for convience if the sender was local */
            if(js_islocal(si, q->p->from))
                u = js_user(si, q->p->from, NULL);

            /* let the modules have a go at the packet; if nobody handles it... */
            if(!js_mapi_call(si, e_SERVER, q->p, u, NULL))
                js_bounce(si,q->p->x,TERROR_NOTFOUND);

        }
    }

    /* shouldn't arrive here, but clean up, just in case */
    pth_event_free(ev,PTH_FREE_ALL);
    pth_msgport_destroy(mp);

}

