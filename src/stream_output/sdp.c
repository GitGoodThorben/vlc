/*****************************************************************************
 * sdp.c : SDP creation helpers
 *****************************************************************************
 * Copyright © 2007 Rémi Denis-Courmont
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <vlc/vlc.h>

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>
#include <vlc_network.h>
#include <vlc_charset.h>

#include "stream_output.h"

#define MAXSDPADDRESS 47

static
char *AddressToSDP (const struct sockaddr *addr, socklen_t addrlen, char *buf)
{
    if (addrlen < offsetof (struct sockaddr, sa_family)
                 + sizeof (addr->sa_family))
        return NULL;

    strcpy (buf, "IN IP* ");

    if (vlc_getnameinfo (addr, addrlen, buf + 7, MAXSDPADDRESS - 7, NULL,
                         NI_NUMERICHOST))
        return NULL;

    switch (addr->sa_family)
    {
        case AF_INET:
        {
            if (net_SockAddrIsMulticast (addr, addrlen))
                strcat (buf, "/255"); // obsolete in RFC4566, dummy value
            buf[5] = '4';
            break;
        }

#ifdef AF_INET6
        case AF_INET6:
        {
            char *ptr = strchr (buf, '%');
            if (ptr != NULL)
                *ptr = '\0'; // remove scope ID
            buf[5] = '6';
            break;
        }
#endif

        default:
            return NULL;
    }

    return buf;
}


static vlc_bool_t IsSDPString (const char *str)
{
    if (strchr (str, '\r') != NULL)
        return VLC_FALSE;
    if (strchr (str, '\n') != NULL)
        return VLC_FALSE;
    if (!IsUTF8 (str))
        return VLC_FALSE;
    return VLC_TRUE;
}


char *sdp_Start (const char *name, const char *description, const char *url,
                const char *email, const char *phone,
                const struct sockaddr *src, socklen_t srclen,
                const struct sockaddr *addr, socklen_t addrlen)
{
    uint64_t now = NTPtime64 ();
    char *sdp;
    char connection[MAXSDPADDRESS], hostname[256],
         sfilter[MAXSDPADDRESS + sizeof ("\r\na=source-filter: incl * ")];
    const char *preurl = "\r\nu=", *premail = "\r\ne=", *prephone = "\r\np=";

    gethostname (hostname, sizeof (hostname));

    if (name == NULL)
        name = "Unnamed";
    if (description == NULL)
        description = "N/A";
    if (url == NULL)
        preurl = url = "";
    if (email == NULL)
        premail = email = "";
    if (phone == NULL)
        prephone = phone = "";

    if (!IsSDPString (name) || !IsSDPString (description)
     || !IsSDPString (url) || !IsSDPString (email) || !IsSDPString (phone)
     || (AddressToSDP (addr, addrlen, connection) == NULL))
        return NULL;

    strcpy (sfilter, "");
    if (srclen > 0)
    {
        char machine[MAXSDPADDRESS];

        if (AddressToSDP (src, srclen, machine) != NULL)
            sprintf (sfilter, "\r\na=source-filter: incl IN IP%c * %s",
                     machine[5], machine + 7);
    }

    if (asprintf (&sdp, "v=0"
                    "\r\no=- "I64Fu" "I64Fu" IN IP%c %s"
                    "\r\ns=%s"
                    "\r\ni=%s"
                    "%s%s" // optional URL
                    "%s%s" // optional email
                    "%s%s" // optional phone number
                    "\r\nc=%s"
                        // bandwidth not specified
                    "\r\nt=0 0" // one dummy time span
                        // no repeating
                        // no time zone adjustment (silly idea anyway)
                        // no encryption key (deprecated)
                    "\r\na=tool:"PACKAGE_STRING
                    "\r\na=recvonly"
                    "\r\na=type:broadcast"
                    "\r\na=charset:UTF-8"
                    "%s" // optional source filter
                    "\r\n",
               /* o= */ now, now, connection[5], hostname,
               /* s= */ name,
               /* i= */ description,
               /* u= */ preurl, url,
               /* e= */ premail, email,
               /* p= */ prephone, phone,
               /* c= */ connection,
    /* source-filter */ sfilter) == -1)
        return NULL;
    return sdp;
}


static char *
vsdp_AddAttribute (char **sdp, const char *name, const char *fmt, va_list ap)
{
    size_t oldlen = strlen (*sdp);
    size_t addlen =
        sizeof ("a=:\r\n") + strlen (name) + vsnprintf (NULL, 0, fmt, ap);
    char *ret = realloc (*sdp, oldlen + addlen);

    if (ret == NULL)
        return NULL;

    oldlen += sprintf (ret + oldlen, "a=%s:", name);
    sprintf (ret + oldlen, fmt, ap);
    return *sdp = ret;
}


char *sdp_AddAttribute (char **sdp, const char *name, const char *fmt, ...)
{
    char *ret;

    if (fmt != NULL)
    {
        va_list ap;

        va_start (ap, fmt);
        ret = vsdp_AddAttribute (sdp, name, fmt, ap);
        va_end (ap);
    }
    else
    {
        size_t oldlen = strlen (*sdp);
        ret = realloc (*sdp, oldlen + strlen (name) + sizeof ("a=\r\n"));
        if (ret == NULL)
            return NULL;

        sprintf (ret + oldlen, "a=%s\r\n", name);
    }
    return ret;
}


char *sdp_AddMedia (char **sdp,
                    const char *type, const char *protocol, int dport,
                    unsigned pt, vlc_bool_t bw_indep, unsigned bw,
                    const char *rtpmap, const char *fmtp)
{
    char *newsdp, *ptr;
    size_t inlen = strlen (*sdp), outlen = inlen;

    /* Some default values */
    if (type == NULL)
        type = "video";
    if (protocol == NULL)
        protocol = "RTP/AVP";
    assert (pt < 128u);

    outlen += snprintf (NULL, 0,
                        "m=%s %u %s %d\r\n"
                        "b=RR:0\r\n",
                        type, dport, protocol, pt);

    newsdp = realloc (*sdp, outlen + 1);
    if (newsdp == NULL)
        return NULL;

    *sdp = newsdp;
    ptr = newsdp + inlen;

    ptr += sprintf (ptr, "m=%s %u %s %d\r\n"
                         "b=RR:0\r\n",
                         type, dport, protocol, pt);

    /* RTP payload type map */
    if (rtpmap != NULL)
        sdp_AddAttribute (sdp, "rtpmap", "%u %s", pt, rtpmap);
    /* Format parameters */
    if (fmtp != NULL)
        sdp_AddAttribute (sdp, "fmtp", "%u %s", pt, fmtp);

    return newsdp;
}
