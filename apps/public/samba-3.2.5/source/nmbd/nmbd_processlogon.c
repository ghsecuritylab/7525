/* 
   Unix SMB/CIFS implementation.
   NBT netbios routines and daemon - version 2
   Copyright (C) Andrew Tridgell 1994-1998
   Copyright (C) Luke Kenneth Casson Leighton 1994-1998
   Copyright (C) Jeremy Allison 1994-2003
   Copyright (C) Jim McDonough <jmcd@us.ibm.com> 2002
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
   
   Revision History:

*/

#include "includes.h"

struct sam_database_info {
        uint32 index;
        uint32 serial_lo, serial_hi;
        uint32 date_lo, date_hi;
};

/****************************************************************************
Process a domain logon packet
**************************************************************************/

void process_logon_packet(struct packet_struct *p, char *buf,int len, 
                          const char *mailslot)
{
	struct dgram_packet *dgram = &p->packet.dgram;
	fstring my_name;
	fstring reply_name;
	char outbuf[1024];
	int code;
	uint16 token = 0;
	uint32 ntversion = 0;
	uint16 lmnttoken = 0;
	uint16 lm20token = 0;
	uint32 domainsidsize;
	bool short_request = False;
	char *getdc;
	char *uniuser; /* Unicode user name. */
	fstring ascuser;
	char *unicomp; /* Unicode computer name. */
	size_t size;
	struct sockaddr_storage ss;
	const struct sockaddr_storage *pss;
	struct in_addr ip;

	in_addr_to_sockaddr_storage(&ss, p->ip);
	pss = iface_ip(&ss);
	if (!pss) {
		DEBUG(5,("process_logon_packet:can't find outgoing interface "
			"for packet from IP %s\n",
			inet_ntoa(p->ip) ));
		return;
	}
	ip = ((struct sockaddr_in *)pss)->sin_addr;

	memset(outbuf, 0, sizeof(outbuf));

	if (!lp_domain_logons()) {
		DEBUG(5,("process_logon_packet: Logon packet received from IP %s and domain \
logons are not enabled.\n", inet_ntoa(p->ip) ));
		return;
	}

	fstrcpy(my_name, global_myname());

	code = get_safe_SVAL(buf,len,buf,0,-1);
	DEBUG(4,("process_logon_packet: Logon from %s: code = 0x%x\n", inet_ntoa(p->ip), code));

	switch (code) {
		case 0:
			{
				fstring mach_str, user_str, getdc_str;
				char *q = buf + 2;
				char *machine = q;
				char *user = skip_string(buf,len,machine);

				if (!user || PTR_DIFF(user, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}
				getdc = skip_string(buf,len,user);

				if (!getdc || PTR_DIFF(getdc, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}
				q = skip_string(buf,len,getdc);

				if (!q || PTR_DIFF(q + 5, buf) > len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}
				token = SVAL(q,3);

				fstrcpy(reply_name,my_name);

				pull_ascii_fstring(mach_str, machine);
				pull_ascii_fstring(user_str, user);
				pull_ascii_fstring(getdc_str, getdc);

				DEBUG(5,("process_logon_packet: Domain login request from %s at IP %s user=%s token=%x\n",
					mach_str,inet_ntoa(p->ip),user_str,token));

				q = outbuf;
				SSVAL(q, 0, 6);
				q += 2;

				fstrcpy(reply_name, "\\\\");
				fstrcat(reply_name, my_name);
				size = push_ascii(q,reply_name,
						sizeof(outbuf)-PTR_DIFF(q, outbuf),
						STR_TERMINATE);
				if (size == (size_t)-1) {
					return;
				}
				q = skip_string(outbuf,sizeof(outbuf),q); /* PDC name */

				SSVAL(q, 0, token);
				q += 2;

				dump_data(4, (uint8 *)outbuf, PTR_DIFF(q, outbuf));

				send_mailslot(True, getdc_str,
						outbuf,PTR_DIFF(q,outbuf),
						global_myname(), 0x0,
						mach_str,
						dgram->source_name.name_type,
						p->ip, ip, p->port);
				break;
			}

		case QUERYFORPDC:
			{
				fstring mach_str, getdc_str;
				fstring source_name;
				char *q = buf + 2;
				char *machine = q;

				if (!lp_domain_master()) {
					/* We're not Primary Domain Controller -- ignore this */
					return;
				}

				getdc = skip_string(buf,len,machine);

				if (!getdc || PTR_DIFF(getdc, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}
				q = skip_string(buf,len,getdc);

				if (!q || PTR_DIFF(q, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}
				q = ALIGN2(q, buf);

				/* At this point we can work out if this is a W9X or NT style
				   request. Experiments show that the difference is wether the
				   packet ends here. For a W9X request we now end with a pair of
				   bytes (usually 0xFE 0xFF) whereas with NT we have two further
				   strings - the following is a simple way of detecting this */

				if (len - PTR_DIFF(q, buf) <= 3) {
					short_request = True;
				} else {
					unicomp = q;

					if (PTR_DIFF(q, buf) >= len) {
						DEBUG(0,("process_logon_packet: bad packet\n"));
						return;
					}

					/* A full length (NT style) request */
					q = skip_unibuf(unicomp, PTR_DIFF(buf + len, unicomp));

					if (PTR_DIFF(q, buf) >= len) {
						DEBUG(0,("process_logon_packet: bad packet\n"));
						return;
					}

					if (len - PTR_DIFF(q, buf) > 8) {
						/* with NT5 clients we can sometimes
							get additional data - a length specificed string
							containing the domain name, then 16 bytes of
							data (no idea what it is) */
						int dom_len = CVAL(q, 0);
						q++;
						if (dom_len != 0) {
							q += dom_len + 1;
						}
						q += 16;
					}

					if (PTR_DIFF(q + 8, buf) > len) {
						DEBUG(0,("process_logon_packet: bad packet\n"));
						return;
					}

					ntversion = IVAL(q, 0);
					lmnttoken = SVAL(q, 4);
					lm20token = SVAL(q, 6);
				}

				/* Construct reply. */
				q = outbuf;
				SSVAL(q, 0, QUERYFORPDC_R);
				q += 2;

				fstrcpy(reply_name,my_name);
				size = push_ascii(q, reply_name,
						sizeof(outbuf)-PTR_DIFF(q, outbuf),
						STR_TERMINATE);
				if (size == (size_t)-1) {
					return;
				}
				q = skip_string(outbuf,sizeof(outbuf),q); /* PDC name */

				/* PDC and domain name */
				if (!short_request) {
					/* Make a full reply */
					q = ALIGN2(q, outbuf);

					q += dos_PutUniCode(q, my_name,
						sizeof(outbuf) - PTR_DIFF(q, outbuf),
						True); /* PDC name */
					q += dos_PutUniCode(q, lp_workgroup(),
						sizeof(outbuf) - PTR_DIFF(q, outbuf),
						True); /* Domain name*/
					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 8) {
						return;
					}
					SIVAL(q, 0, 1); /* our nt version */
					SSVAL(q, 4, 0xffff); /* our lmnttoken */
					SSVAL(q, 6, 0xffff); /* our lm20token */
					q += 8;
				}

				/* RJS, 21-Feb-2000, we send a short reply if the request was short */

				pull_ascii_fstring(mach_str, machine);

				DEBUG(5,("process_logon_packet: GETDC request from %s at IP %s, \
reporting %s domain %s 0x%x ntversion=%x lm_nt token=%x lm_20 token=%x\n",
					mach_str,inet_ntoa(p->ip), reply_name, lp_workgroup(),
					QUERYFORPDC_R, (uint32)ntversion, (uint32)lmnttoken,
					(uint32)lm20token ));

				dump_data(4, (uint8 *)outbuf, PTR_DIFF(q, outbuf));

				pull_ascii_fstring(getdc_str, getdc);
				pull_ascii_nstring(source_name, sizeof(source_name), dgram->source_name.name);

				send_mailslot(True, getdc_str,
					outbuf,PTR_DIFF(q,outbuf),
					global_myname(), 0x0,
					source_name,
					dgram->source_name.name_type,
					p->ip, ip, p->port);
				return;
			}

		case SAMLOGON:

			{
				fstring getdc_str;
				fstring source_name;
				char *q = buf + 2;
				fstring asccomp;

				q += 2;

				if (PTR_DIFF(q, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}

				unicomp = q;
				uniuser = skip_unibuf(unicomp, PTR_DIFF(buf+len, unicomp));

				if (PTR_DIFF(uniuser, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}

				getdc = skip_unibuf(uniuser,PTR_DIFF(buf+len, uniuser));

				if (PTR_DIFF(getdc, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}

				q = skip_string(buf,len,getdc);

				if (!q || PTR_DIFF(q + 8, buf) >= len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}

				q += 4; /* Account Control Bits - indicating username type */
				domainsidsize = IVAL(q, 0);
				q += 4;

				DEBUG(5,("process_logon_packet: SAMLOGON sidsize %d, len = %d\n", domainsidsize, len));

				if (domainsidsize < (len - PTR_DIFF(q, buf)) && (domainsidsize != 0)) {
					q += domainsidsize;
					q = ALIGN4(q, buf);
				}

				DEBUG(5,("process_logon_packet: len = %d PTR_DIFF(q, buf) = %ld\n", len, (unsigned long)PTR_DIFF(q, buf) ));

				if (len - PTR_DIFF(q, buf) > 8) {
					/* with NT5 clients we can sometimes
						get additional data - a length specificed string
						containing the domain name, then 16 bytes of
						data (no idea what it is) */
					int dom_len = CVAL(q, 0);
					q++;
					if (dom_len < (len - PTR_DIFF(q, buf)) && (dom_len != 0)) {
						q += dom_len + 1;
					}
					q += 16;
				}

				if (PTR_DIFF(q + 8, buf) > len) {
					DEBUG(0,("process_logon_packet: bad packet\n"));
					return;
				}

				ntversion = IVAL(q, 0);
				lmnttoken = SVAL(q, 4);
				lm20token = SVAL(q, 6);
				q += 8;

				DEBUG(3,("process_logon_packet: SAMLOGON sidsize %d ntv %d\n", domainsidsize, ntversion));

				/*
				 * we respond regadless of whether the machine is in our password 
				 * database. If it isn't then we let smbd send an appropriate error.
				 * Let's ignore the SID.
				 */
				pull_ucs2_fstring(ascuser, uniuser);
				pull_ucs2_fstring(asccomp, unicomp);
				DEBUG(5,("process_logon_packet: SAMLOGON user %s\n", ascuser));

				fstrcpy(reply_name, "\\\\"); /* Here it wants \\LOGONSERVER. */
				fstrcat(reply_name, my_name);

				DEBUG(5,("process_logon_packet: SAMLOGON request from %s(%s) for %s, returning logon svr %s domain %s code %x token=%x\n",
					asccomp,inet_ntoa(p->ip), ascuser, reply_name, lp_workgroup(),
				SAMLOGON_R ,lmnttoken));

				/* Construct reply. */

				q = outbuf;
				/* we want the simple version unless we are an ADS PDC..which means  */
				/* never, at least for now */
				if ((ntversion < 11) || (SEC_ADS != lp_security()) || (ROLE_DOMAIN_PDC != lp_server_role())) {
					if (SVAL(uniuser, 0) == 0) {
						SSVAL(q, 0, SAMLOGON_UNK_R);	/* user unknown */
					} else {
						SSVAL(q, 0, SAMLOGON_R);
					}

					q += 2;

					q += dos_PutUniCode(q, reply_name,
						sizeof(outbuf) - PTR_DIFF(q, outbuf),
						True);
					q += dos_PutUniCode(q, ascuser,
						sizeof(outbuf) - PTR_DIFF(q, outbuf),
						True);
					q += dos_PutUniCode(q, lp_workgroup(),
						sizeof(outbuf) - PTR_DIFF(q, outbuf),
						True);
				}
#ifdef HAVE_ADS
				else {
					struct GUID domain_guid;
					UUID_FLAT flat_guid;
					char *domain;
					char *hostname;
					char *component, *dc, *q1;
					char *q_orig = q;
					int str_offset;
					char *saveptr = NULL;

					domain = get_mydnsdomname(talloc_tos());
					if (!domain) {
						DEBUG(2,
						("get_mydnsdomname failed.\n"));
						return;
					}
					hostname = get_myname(talloc_tos());
					if (!hostname) {
						DEBUG(2,
						("get_myname failed.\n"));
						return;
					}

					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 8) {
						return;
					}
					if (SVAL(uniuser, 0) == 0) {
						SIVAL(q, 0, SAMLOGON_AD_UNK_R);	/* user unknown */
					} else {
						SIVAL(q, 0, SAMLOGON_AD_R);
					}
					q += 4;

					SIVAL(q, 0, NBT_SERVER_PDC|NBT_SERVER_GC|NBT_SERVER_LDAP|NBT_SERVER_DS|
						NBT_SERVER_KDC|NBT_SERVER_TIMESERV|NBT_SERVER_CLOSEST|NBT_SERVER_WRITABLE);
					q += 4;

					/* Push Domain GUID */
					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < UUID_FLAT_SIZE) {
						return;
					}
					if (False == secrets_fetch_domain_guid(domain, &domain_guid)) {
						DEBUG(2, ("Could not fetch DomainGUID for %s\n", domain));
						return;
					}

					smb_uuid_pack(domain_guid, &flat_guid);
					memcpy(q, &flat_guid.info, UUID_FLAT_SIZE);
					q += UUID_FLAT_SIZE;

					/* Forest */
					str_offset = q - q_orig;
					dc = domain;
					q1 = q;
					while ((component = strtok_r(dc, ".", &saveptr)) != NULL) {
						dc = NULL;
						if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 1) {
							return;
						}
						size = push_ascii(&q[1], component,
							sizeof(outbuf) - PTR_DIFF(q+1, outbuf),
							0);
						if (size == (size_t)-1 || size > 0xff) {
							return;
						}
						SCVAL(q, 0, size);
						q += (size + 1);
					}

					/* Unk0 */
					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 4) {
						return;
					}
					SCVAL(q, 0, 0);
					q++;

					/* Domain */
					SCVAL(q, 0, 0xc0 | ((str_offset >> 8) & 0x3F));
					SCVAL(q, 1, str_offset & 0xFF);
					q += 2;

					/* Hostname */
					size = push_ascii(&q[1], hostname,
							sizeof(outbuf) - PTR_DIFF(q+1, outbuf),
							0);
					if (size == (size_t)-1 || size > 0xff) {
						return;
					}
					SCVAL(q, 0, size);
					q += (size + 1);

					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 3) {
						return;
					}

					SCVAL(q, 0, 0xc0 | ((str_offset >> 8) & 0x3F));
					SCVAL(q, 1, str_offset & 0xFF);
					q += 2;

					/* NETBIOS of domain */
					size = push_ascii(&q[1], lp_workgroup(),
							sizeof(outbuf) - PTR_DIFF(q+1, outbuf),
							STR_UPPER);
					if (size == (size_t)-1 || size > 0xff) {
						return;
					}
					SCVAL(q, 0, size);
					q += (size + 1);

					/* Unk1 */
					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 2) {
						return;
					}

					SCVAL(q, 0, 0);
					q++;

					/* NETBIOS of hostname */
					size = push_ascii(&q[1], my_name,
							sizeof(outbuf) - PTR_DIFF(q+1, outbuf),
							0);
					if (size == (size_t)-1 || size > 0xff) {
						return;
					}
					SCVAL(q, 0, size);
					q += (size + 1);

					/* Unk2 */
					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 4) {
						return;
					}

					SCVAL(q, 0, 0);
					q++;

					/* User name */
					if (SVAL(uniuser, 0) != 0) {
						size = push_ascii(&q[1], ascuser,
							sizeof(outbuf) - PTR_DIFF(q+1, outbuf),
							0);
						if (size == (size_t)-1 || size > 0xff) {
							return;
						}
						SCVAL(q, 0, size);
						q += (size + 1);
					}

					q_orig = q;
					/* Site name */
					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 1) {
						return;
					}
					size = push_ascii(&q[1], "Default-First-Site-Name",
							sizeof(outbuf) - PTR_DIFF(q+1, outbuf),
							0);
					if (size == (size_t)-1 || size > 0xff) {
						return;
					}
					SCVAL(q, 0, size);
					q += (size + 1);

					if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 18) {
						return;
					}

					/* Site name (2) */
					str_offset = q - q_orig;
					SCVAL(q, 0, 0xc0 | ((str_offset >> 8) & 0x3F));
					SCVAL(q, 1, str_offset & 0xFF);
					q += 2;

					SCVAL(q, 0, PTR_DIFF(q,q1));
					SCVAL(q, 1, 0x10); /* unknown */

					SIVAL(q, 0, 0x00000002);
					q += 4; /* unknown */
					SIVAL(q, 0, ntohl(ip.s_addr));
					q += 4;
					SIVAL(q, 0, 0x00000000);
					q += 4; /* unknown */
					SIVAL(q, 0, 0x00000000);
					q += 4; /* unknown */
				}
#endif

				if (sizeof(outbuf) - PTR_DIFF(q, outbuf) < 8) {
					return;
				}

				/* tell the client what version we are */
				SIVAL(q, 0, ((ntversion < 11) || (SEC_ADS != lp_security())) ? 1 : 13); 
				/* our ntversion */
				SSVAL(q, 4, 0xffff); /* our lmnttoken */
				SSVAL(q, 6, 0xffff); /* our lm20token */
				q += 8;

				dump_data(4, (uint8 *)outbuf, PTR_DIFF(q, outbuf));

				pull_ascii_fstring(getdc_str, getdc);
				pull_ascii_nstring(source_name, sizeof(source_name), dgram->source_name.name);

				send_mailslot(True, getdc,
					outbuf,PTR_DIFF(q,outbuf),
					global_myname(), 0x0,
					source_name,
					dgram->source_name.name_type,
					p->ip, ip, p->port);
				break;
			}

		/* Announce change to UAS or SAM.  Send by the domain controller when a
		replication event is required. */

		case SAM_UAS_CHANGE:
			DEBUG(5, ("Got SAM_UAS_CHANGE\n"));
			break;

		default:
			DEBUG(3,("process_logon_packet: Unknown domain request %d\n",code));
			return;
	}
}
