/*
 *  Copyright (C) 2014 Cisco Systems, Inc.
 *
 *  Authors: Steven Morgan <smorgan@sourcefire.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

#if HAVE_LIBXML2
#ifdef _WIN32
#ifndef LIBXML_WRITER_ENABLED
#define LIBXML_WRITER_ENABLED 1
#endif
#endif
#include <libxml/xmlreader.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "libclamav/crypto.h"
#include "others.h"
#include "openioc.h"

struct openioc_hash {
    unsigned char * hash;
    void * next;
};

static const xmlChar * openioc_read(xmlTextReaderPtr reader)
{
    const xmlChar * name;
    if (xmlTextReaderRead(reader) != 1)
        return NULL;
    name = xmlTextReaderConstLocalName(reader);
    if (name != NULL) {
        cli_dbgmsg("cli_openioc: xmlTextReaderRead read %s%s\n", name,
                   xmlTextReaderNodeType(reader) == XML_READER_TYPE_END_ELEMENT?" end tag":"");
    }
    return name;   
}

static int openioc_parse_content(xmlTextReaderPtr reader, struct openioc_hash ** elems)
{
    xmlChar * type = xmlTextReaderGetAttribute(reader, (const xmlChar *)"type");
    const xmlChar * xmlval;
    struct openioc_hash * elem;
    int rc = CL_SUCCESS;

    if (type == NULL) {
        cli_dbgmsg("cli_openioc: xmlTextReaderGetAttribute no type attribute "
                   "for <Content> element\n");
        return rc;
    } else { 
        if (xmlStrcasecmp(type, (const xmlChar *)"sha1") &&
            xmlStrcasecmp(type, (const xmlChar *)"sha256") &&
            xmlStrcasecmp(type, (const xmlChar *)"md5")) {
            xmlFree(type);
            return rc;
        }
    }
    xmlFree(type);
    
    if (xmlTextReaderRead(reader) == 1 && xmlTextReaderNodeType(reader) == XML_READER_TYPE_TEXT) {
        xmlval = xmlTextReaderConstValue(reader);
        if (xmlval) {
            elem = cli_calloc(1, sizeof(struct openioc_hash));
            if (NULL == elem) {
                cli_dbgmsg("cli_openioc: calloc fails for openioc_hash.\n");
                return CL_EMEM;
            }
            elem->hash = xmlStrdup(xmlval);
            elem->next = *elems;
            *elems = elem; 
        } else {
            cli_dbgmsg("cli_openioc: xmlTextReaderConstValue() returns NULL for Content md5 value.\n");           
        }
    }
    else {
        cli_dbgmsg("cli_openioc: No text for XML Content element.\n");
    }
    return rc;
}

static int openioc_parse_indicatoritem(xmlTextReaderPtr reader, struct openioc_hash ** elems)
{
    const xmlChar * name;
    int rc = CL_SUCCESS;

    while (1) {
        name = openioc_read(reader);
        if (name == NULL)
            break;
        if (xmlStrEqual(name, (const xmlChar *)"Content") && 
            xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) {
            rc = openioc_parse_content(reader, elems);
            if (rc != CL_SUCCESS) {
                cli_dbgmsg("cli_openioc: openioc_parse_content error.\n");
                break;
            }
        } else if (xmlStrEqual(name, (const xmlChar *)"IndicatorItem") &&
                   xmlTextReaderNodeType(reader) == XML_READER_TYPE_END_ELEMENT) {
            break;
        }
    }
    return rc;
}

static int openioc_parse_indicator(xmlTextReaderPtr reader, struct openioc_hash ** elems)
{
    const xmlChar * name;
    int rc = CL_SUCCESS;

    while (1) {
        name = openioc_read(reader);
        if (name == NULL)
            return rc;
        if (xmlStrEqual(name, (const xmlChar *)"Indicator") && 
            xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) {
            rc = openioc_parse_indicator(reader, elems);
            if (rc != CL_SUCCESS) {
                cli_dbgmsg("cli_openioc: openioc_parse_indicator recursion error.\n");
                break;
            }
        } else if (xmlStrEqual(name, (const xmlChar *)"IndicatorItem") && 
            xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) {
            rc = openioc_parse_indicatoritem(reader, elems);
            if (rc != CL_SUCCESS) {
                cli_dbgmsg("cli_openioc: openioc_parse_indicatoritem error.\n");
                break;
            }
        } else if (xmlStrEqual(name, (const xmlChar *)"Indicator") &&
                   xmlTextReaderNodeType(reader) == XML_READER_TYPE_END_ELEMENT) {
            break;
        }
    }
    return rc;
}

int openioc_parse(const char * fname, int fd, struct cl_engine *engine)
{
    int rc;
    xmlTextReaderPtr reader = NULL;
    const xmlChar * name;
    struct openioc_hash * elems = NULL, * elem = NULL;
    const char * iocp = NULL;
    char iocname[MAXPATHLEN] = {0};
    uint16_t ioclen;
    char * virusname;
    
    if (fname == NULL)
        return CL_ENULLARG;

    if (fd < 0)
        return CL_EARG;

    cli_dbgmsg("cli_openioc: XML parsing file %s\n", fname);

    reader = xmlReaderForFd(fd, NULL, NULL, 0);
    if (reader == NULL) {
        cli_dbgmsg("cli_openioc: xmlReaderForFd error\n");
        return CL_EOPEN;
    }
    rc = xmlTextReaderRead(reader);
    while (rc == 1) {
        name = xmlTextReaderConstLocalName(reader);
        cli_dbgmsg("cli_openioc: xmlTextReaderRead read %s\n", name);
        if (xmlStrEqual(name, (const xmlChar *)"Indicator") && 
            xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) {
            rc = openioc_parse_indicator(reader, &elems);
            if (rc != CL_SUCCESS) {
                cli_dbgmsg("cli_openioc: openioc_parse_indicator error.\n");
                return rc;
            }
        }
        if (xmlStrEqual(name, (const xmlChar *)"ioc") &&
            xmlTextReaderNodeType(reader) == XML_READER_TYPE_END_ELEMENT) {
            break;
        }
        rc = xmlTextReaderRead(reader);
    }

    iocp = strrchr(fname, *PATHSEP);

    if (NULL == iocp)
        iocp = fname;
    else
        iocp++;

    strncpy(iocname, iocp, MAXPATHLEN-1);
    ioclen = strlen(iocname);

    if (elems != NULL) {
        if (NULL == engine->hm_hdb) {
            engine->hm_hdb = mpool_calloc(engine->mempool, 1, sizeof(struct cli_matcher));
            if (NULL == engine->hm_hdb)            
                return CL_EMEM;
#ifdef USE_MPOOL
            engine->hm_hdb->mempool = engine->mempool;
#endif
        }
    }

    while (elems != NULL) {
        char * hash, * sp, * vp;
        int i, hashlen;

        elem = elems;
        elems = elems->next;
        hash = elem->hash;
        while (isspace(*hash))
            hash++;
        hashlen = strlen(hash);
        if (hashlen == 0) {
            xmlFree(elem->hash);
            free(elem);
            continue;
        }
        sp = hash+hashlen-1;
        while (isspace(*sp) && sp > hash) {
            *sp-- = '\0';
            hashlen--;
        }
        virusname = cli_malloc(ioclen+hashlen+2);
        if (NULL == virusname) {
            cli_dbgmsg("cli_openioc: malloc virname failed.\n");
            return CL_EMEM;
        }
        vp = virusname;
        sp = iocname;
        for (i=0; i<ioclen; i++, sp++, vp++) {
            switch (*sp) {
            case '\\':
            case '/':
            case '?':
            case '%':
            case '*':
            case ':':
            case '|':
            case '"':
            case '<':
            case '>':
                *vp = '_';
            default:
                if (isspace(*sp))
                    *vp = '_';
                else
                    *vp = *sp;
            }
        }
        *vp++ = '.';
        sp = hash;
        for (i=0; i<hashlen; i++, sp++) {
            if (isxdigit(*sp)) {
                *vp++ = *sp;
            }
        }
        *vp = '\0';
        rc = hm_addhash_str(engine->hm_hdb, hash, 0, virusname);
        if (rc != CL_SUCCESS) {
            cli_dbgmsg("cli_openioc: hm_addhash_str failed with %i hash len %i for %s.\n",
                       rc, hashlen, virusname);
        }
        xmlFree(elem->hash);
        free(elem);
    }
}