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
 
#include "jabberd.h"

/* actually filter the packet */
result base_ns_filter(instance i, dpacket p, void *arg)
{
    xmlnode x = (xmlnode)arg;

    /* check all the <ns>...</ns> elements, success if any one of them matches */
    for(x = xmlnode_get_firstchild(x); x != NULL; x = xmlnode_get_nextsibling(x))
        if(j_strcmp(p->id->resource, xmlnode_get_data(x)) == 0)
            return r_PASS;

    return r_LAST;
}

result base_ns_config(instance id, xmlnode x, void *arg)
{
    xmlnode ns;

    if(id == NULL)
    {
        log_debug(ZONE,"base_ns_config validating configuration\n");
        if(xmlnode_get_data(x) == NULL)
        {
            xmlnode_put_attrib(x,"error","'ns' tag must contain a valid Namespace to filter by\nSuch as: jabber:iq:auth");
            return r_ERR;
        }
        return r_PASS;
    }

    log_debug(ZONE,"base_ns_config performing configuration %s\n",xmlnode2str(x));

    /* XXX uhgly, should figure out how to flag errors like this more consistently, during checking phase or something */
    if(id->type != p_XDB)
    {
        fprintf(stderr,"ERROR: <ns>...</ns> element only allowed in xdb section\n");
        exit(1);
    }

    /* hack hack away, ugly but effective... we need to correlate all the <ns> elements within an instance, hide them together in an xmlnode in an attrib on the id */
    ns = (xmlnode)xmlnode_get_vattrib(id->x, "base_ns");
    if(ns == NULL)
    {
        ns = xmlnode_new_tag_pool(xmlnode_pool(id->x),"ns");
        xmlnode_put_vattrib(id->x,"base_ns",(void *)ns);
    }
    xmlnode_insert_tag_node(ns,x);

    register_phandler(id, o_COND, base_ns_filter, (void *)ns);

    return r_DONE;
}

void base_ns(void)
{
    log_debug(ZONE,"base_ns loading...\n");

    register_config("ns",base_ns_config,NULL);
}