/*
 * Copyright (c) 1997 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. All advertising materials mentioning features or use of this software 
 *    must display the following acknowledgement: 
 *      This product includes software developed by Kungliga Tekniska 
 *      H�gskolan and its contributors. 
 *
 * 4. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "kdc_locl.h"

RCSID("$Id$");

#ifdef KRB4

static u_int32_t
swap32(u_int32_t x)
{
    return (x << 24) & 0xff000000 |
	(x << 8) & 0xff0000 |
	(x >> 8) & 0xff00 |
	(x >> 24) & 0xff;
}

int
maybe_version4(unsigned char *buf, int len)
{
    return len > 0 && *buf == 4;
}

static void
make_err_reply(krb5_data *reply, int code, const char *msg)
{
    KTEXT_ST er;

    /* name, instance and realm is not checked in most (all?) version
       implementations; msg is also never used, but we send it anyway
       (for debugging purposes) */

    if(msg == NULL)
	msg = krb_get_err_text(code);
    cr_err_reply(&er, "", "", "", kdc_time, code, (char*)msg);
    krb5_data_copy(reply, er.dat, er.length);
}

#define RCHECK(X, L) if(X){make_err_reply(reply, KFAILURE, "Packet too short"); goto L;}

krb5_error_code
do_version4(unsigned char *buf,
	    size_t len,
	    krb5_data *reply,
	    const char *from,
	    struct sockaddr_in *addr)
{
    krb5_storage *sp;
    krb5_error_code ret;
    krb5_principal client_princ = NULL, server_princ = NULL;
    hdb_entry *client = NULL, *server = NULL;
    Key *ckey, *skey, *ekey;
    int8_t pvno;
    int8_t msg_type;
    int lsb;
    char *name = NULL, *inst = NULL, *realm = NULL;
    char *sname = NULL, *sinst = NULL;
    int32_t req_time;
    time_t max_life;
    u_int8_t life;

    sp = krb5_storage_from_mem(buf, len);
    RCHECK(krb5_ret_int8(sp, &pvno), out);
    if(pvno != 4){
	kdc_log(0, "Protocol version mismatch (%d)", pvno);
	make_err_reply(reply, KDC_PKT_VER, NULL);
	goto out;
    }
    RCHECK(krb5_ret_int8(sp, &msg_type), out);
    lsb = msg_type & 1;
    msg_type &= ~1;
    switch(msg_type){
    case AUTH_MSG_KDC_REQUEST:
	RCHECK(krb5_ret_stringz(sp, &name), out1);
	RCHECK(krb5_ret_stringz(sp, &inst), out1);
	RCHECK(krb5_ret_stringz(sp, &realm), out1);
	RCHECK(krb5_ret_int32(sp, &req_time), out1);
	if(lsb)
	    req_time = swap32(req_time);
	RCHECK(krb5_ret_int8(sp, &life), out1);
	RCHECK(krb5_ret_stringz(sp, &sname), out1);
	RCHECK(krb5_ret_stringz(sp, &sinst), out1);
	kdc_log(0, "AS-REQ %s.%s@%s from %s for %s.%s", 
		name, inst, realm, from, sname, sinst);

	ret = krb5_425_conv_principal(context, name, inst, realm,
				      &client_princ);

	if(ret){
	    kdc_log(0, "Converting client principal: %s", 
		    krb5_get_err_text(context, ret));
	    make_err_reply(reply, KFAILURE, 
			   "Failed to convert v4 principal (client)");
	    goto out1;
	}

	ret = krb5_425_conv_principal(context, sname, sinst, v4_realm,
				      &server_princ);
	if(ret){
	    kdc_log(0, "Converting server principal: %s", 
		    krb5_get_err_text(context, ret));
	    make_err_reply(reply, KFAILURE, 
			   "Failed to convert v4 principal (server)");
	    goto out1;
	}

	client = db_fetch(client_princ);
	if(client == NULL){
	    kdc_log(0, "Client not found in database: %s.%s@%s", 
		    name, inst, realm);
	    make_err_reply(reply, KERB_ERR_PRINCIPAL_UNKNOWN, NULL);
	    goto out1;
	}
	server = db_fetch(server_princ);
	if(server == NULL){
	    kdc_log(0, "Server not found in database: %s.%s@%s", 
		    sname, sinst, v4_realm);
	    make_err_reply(reply, KERB_ERR_PRINCIPAL_UNKNOWN, NULL);
	    goto out1;
	}

	ret = hdb_keytype2key(context, client, KEYTYPE_DES, &ckey);
	if(ret){
	    kdc_log(0, "%s", krb5_get_err_text(context, ret));
	    /* XXX */
	    make_err_reply(reply, KDC_NULL_KEY, 
			   "No DES key in database (client)");
	    goto out1;
	}

#if 0
	/* this is not necessary with the new code in libkrb */
	/* find a properly salted key */
	while(ckey->salt == NULL || ckey->salt->length != 0)
	    ret = hdb_next_keytype2key(context, client, KEYTYPE_DES, &ckey);
	if(ret){
	    kdc_log(0, "No version-4 salted key in database -- %s.%s@%s", 
		    name, inst, realm);
	    make_err_reply(reply, KDC_NULL_KEY, 
			   "No version-4 salted key in database");
	    goto out1;
	}
#endif
	
	ret = hdb_keytype2key(context, server, KEYTYPE_DES, &skey);
	if(ret){
	    kdc_log(0, "%s", krb5_get_err_text(context, ret));
	    /* XXX */
	    make_err_reply(reply, KDC_NULL_KEY, 
			   "No DES key in database (server)");
	    goto out1;
	}

	max_life = krb_life_to_time(0, life);
	if(client->max_life)
	    max_life = min(max_life, *client->max_life);
	if(server->max_life)
	    max_life = min(max_life, *server->max_life);

	life = krb_time_to_life(kdc_time, kdc_time + max_life);
    
	{
	    KTEXT_ST cipher, ticket;
	    KTEXT r;
	    des_cblock session;

	    des_new_random_key(&session);
	    ekey = unseal_key(skey);

	    krb_create_ticket(&ticket, 0, name, inst, v4_realm,
			      addr->sin_addr.s_addr, session, life, kdc_time, 
			      sname, sinst, ekey->key.keyvalue.data);
	    hdb_free_key(ekey);
	
	    ekey = unseal_key(ckey);
	    create_ciph(&cipher, session, sname, sinst, v4_realm,
			life, server->kvno, &ticket, kdc_time, 
			ekey->key.keyvalue.data);
	    hdb_free_key(ekey);
	    memset(&session, 0, sizeof(session));
	    r = create_auth_reply(name, inst, realm, req_time, 0, 
				  client->pw_end ? *client->pw_end : 0, 
				  client->kvno, &cipher);
	    krb5_data_copy(reply, r->dat, r->length);
	    memset(&cipher, 0, sizeof(cipher));
	    memset(&ticket, 0, sizeof(ticket));
	}
    out1:
	break;
    case AUTH_MSG_APPL_REQUEST: {
	int8_t kvno;
	int8_t ticket_len;
	int8_t req_len;
	KTEXT_ST auth;
	AUTH_DAT ad;
	size_t pos;
	krb5_principal tgt_princ = NULL;
	hdb_entry *tgt = NULL;
	Key *tkey;
	
	RCHECK(krb5_ret_int8(sp, &kvno), out2);
	RCHECK(krb5_ret_stringz(sp, &realm), out2);
	
	ret = krb5_425_conv_principal(context, "krbtgt", realm, v4_realm,
				      &tgt_princ);
	if(ret){
	    kdc_log(0, "Converting krbtgt principal: %s", 
		    krb5_get_err_text(context, ret));
	    make_err_reply(reply, KFAILURE, 
			   "Failed to convert v4 principal (krbtgt)");
	    goto out2;
	}

	tgt = db_fetch(tgt_princ);
	if(tgt == NULL){
	    char *s;
	    s = kdc_log_msg(0, "Ticket-granting ticket not "
			    "found in database: krbtgt.%s@%s", 
			    realm, v4_realm);
	    make_err_reply(reply, KFAILURE, s);
	    free(s);
	    goto out2;
	}
	
	if(tgt->kvno != kvno){
	    goto out2;
	}

	ret = hdb_keytype2key(context, tgt, KEYTYPE_DES, &tkey);
	if(ret){
	    kdc_log(0, "%s", krb5_get_err_text(context, ret));
	    /* XXX */
	    make_err_reply(reply, KDC_NULL_KEY, 
			   "No DES key in database (krbtgt)");
	    goto out2;
	}
	

	RCHECK(krb5_ret_int8(sp, &ticket_len), out2);
	RCHECK(krb5_ret_int8(sp, &req_len), out2);
	
	pos = sp->seek(sp, ticket_len + req_len, SEEK_CUR);
	
	memset(&auth, 0, sizeof(auth));
	memcpy(&auth.dat, buf, pos);
	auth.length = pos;
	ekey = unseal_key(tkey);
	krb_set_key(ekey->key.keyvalue.data, 0);
	hdb_free_key(ekey);
	{
	    int e;
	    e = krb_rd_req(&auth, "krbtgt", realm, 
			   addr->sin_addr.s_addr, &ad, 0);
	    if(e){
		kdc_log(0, "krb_rd_req: %s", krb_get_err_text(e));
		make_err_reply(reply, ret, NULL);
		goto out2;
	    }
	}
	
	RCHECK(krb5_ret_int32(sp, &req_time), out2);
	if(lsb)
	    req_time = swap32(req_time);
	RCHECK(krb5_ret_int8(sp, &life), out2);
	RCHECK(krb5_ret_stringz(sp, &sname), out2);
	RCHECK(krb5_ret_stringz(sp, &sinst), out2);
	kdc_log(0, "TGS-REQ %s.%s@%s from %s for %s.%s", 
		ad.pname, ad.pinst, ad.prealm, from, sname, sinst);
	
	if(strcmp(ad.prealm, realm)){
	    kdc_log(0, "Can't hop realms %s -> %s", realm, ad.prealm);
	    make_err_reply(reply, KERB_ERR_PRINCIPAL_UNKNOWN, 
			   "Can't hop realms");
	    goto out2;
	}

	if(strcmp(sname, "changepw") == 0){
	    kdc_log(0, "Bad request for changepw ticket");
	    make_err_reply(reply, KERB_ERR_PRINCIPAL_UNKNOWN, 
			   "Can't authorize password change based on TGT");
	    goto out2;
	}
	
	ret = krb5_425_conv_principal(context, ad.pname, ad.pinst, ad.prealm, 
				      &client_princ);
	if(ret){
	    kdc_log(0, "Converting client principal: %s", 
		    krb5_get_err_text(context, ret));
	    make_err_reply(reply, KFAILURE, 
			   "Failed to convert v4 principal (client)");
	    goto out2;
	}

	client = db_fetch(client_princ);
	if(client == NULL){
	    char *s;
	    s = kdc_log_msg(0, "Client not found in database: %s.%s@%s", 
			    ad.pname, ad.pinst, ad.prealm);
	    make_err_reply(reply, KERB_ERR_PRINCIPAL_UNKNOWN, s);
	    free(s);
	    goto out2;
	}
	
	ret = krb5_425_conv_principal(context, sname, sinst, v4_realm, 
				      &server_princ);
	if(ret){
	    kdc_log(0, "Converting server principal: %s", 
		    krb5_get_err_text(context, ret));
	    make_err_reply(reply, KFAILURE, 
			   "Failed to convert v4 principal (server)");
	    goto out2;
	}
	server = db_fetch(server_princ);
	if(server == NULL){
	    char *s;
	    s = kdc_log_msg(0, "Server not found in database: %s.%s@%s", 
			    sname, sinst, v4_realm);
	    make_err_reply(reply, KERB_ERR_PRINCIPAL_UNKNOWN, s);
	    free(s);
	    goto out2;
	}

	ret = hdb_keytype2key(context, server, KEYTYPE_DES, &skey);
	if(ret){
	    kdc_log(0, "%s", krb5_get_err_text(context, ret));
	    /* XXX */
	    make_err_reply(reply, KDC_NULL_KEY, 
			   "No DES key in database (server)");
	    goto out2;
	}

	max_life = krb_life_to_time(ad.time_sec, ad.life);
	max_life = min(max_life, krb_life_to_time(kdc_time, life));
	life = min(life, krb_time_to_life(kdc_time, max_life));
	max_life = krb_life_to_time(0, life);
	if(client->max_life)
	    max_life = min(max_life, *client->max_life);
	if(server->max_life)
	    max_life = min(max_life, *server->max_life);
	
	{
	    KTEXT_ST cipher, ticket;
	    KTEXT r;
	    des_cblock session;
	    des_new_random_key(&session);
	    ekey = unseal_key(skey);
	    krb_create_ticket(&ticket, 0, ad.pname, ad.pinst, ad.prealm,
			      addr->sin_addr.s_addr, &session, life, kdc_time,
			      sname, sinst, ekey->key.keyvalue.data);
	    hdb_free_key(ekey);
	    
	    create_ciph(&cipher, session, sname, sinst, v4_realm,
			life, server->kvno, &ticket,
			kdc_time, &ad.session);

	    memset(&session, 0, sizeof(session));
	    memset(ad.session, 0, sizeof(ad.session));

	    r = create_auth_reply(ad.pname, ad.pinst, ad.prealm, 
				  req_time, 0, 0, 0, &cipher);
	    krb5_data_copy(reply, r->dat, r->length);
	    memset(&cipher, 0, sizeof(cipher));
	    memset(&ticket, 0, sizeof(ticket));
	}
    out2:
	if(tgt_princ)
	    krb5_free_principal(context, tgt_princ);
	if(tgt){
	    hdb_free_entry(context, tgt);
	    free(tgt);
	}

	break;
    }
    
    case AUTH_MSG_ERR_REPLY:
	break;
    default:
	kdc_log(0, "Unknown message type: %d from %s", 
		msg_type, from);
	
	make_err_reply(reply, KFAILURE, "Unknown message type");
    }
out:
    if(name)
	free(name);
    if(inst)
	free(inst);
    if(realm)
	free(realm);
    if(sname)
	free(sname);
    if(sinst)
	free(sinst);
    if(client_princ)
	krb5_free_principal(context, client_princ);
    if(server_princ)
	krb5_free_principal(context, server_princ);
    if(client){
	hdb_free_entry(context, client);
	free(client);
    }
    if(server){
	hdb_free_entry(context, server);
	free(server);
    }
    krb5_storage_free(sp);
    return 0;
}


#define ETYPE_DES_PCBC 17 /* XXX */

krb5_error_code
encrypt_v4_ticket(void *buf, size_t len, des_cblock *key, EncryptedData *reply)
{
    des_key_schedule schedule;

    reply->etype = ETYPE_DES_PCBC;
    reply->kvno = NULL;
    reply->cipher.length = len;
    reply->cipher.data = malloc(len);
    if(reply->cipher.data == NULL)
	return ENOMEM;
    des_set_key(key, schedule);
    des_pcbc_encrypt(buf,
		     reply->cipher.data,
		     len,
		     schedule,
		     key,
		     DES_ENCRYPT);
    memset(schedule, 0, sizeof(schedule));
    return 0;
}

krb5_error_code
encode_v4_ticket(void *buf, size_t len, EncTicketPart *et, 
		 PrincipalName *service, size_t *size)
{
    unsigned char *p = buf;
    krb5_storage *sp;
    krb5_error_code ret;
    char name[40], inst[40], realm[40];
    char sname[40], sinst[40];

    {
	krb5_principal princ;
	principalname2krb5_principal(&princ,
				     *service,
				     et->crealm);
	ret = krb5_524_conv_principal(context, 
				      princ,
				      sname,
				      sinst,
				      realm);
	krb5_free_principal(context, princ);
	if(ret)
	    return ret;

	principalname2krb5_principal(&princ,
				     et->cname,
				     et->crealm);
				     
	ret = krb5_524_conv_principal(context, 
				      princ,
				      name,
				      inst,
				      realm);
	krb5_free_principal(context, princ);
    }
    if(ret)
	return ret;

    sp = krb5_storage_emem();
    
    krb5_store_int8(sp, 0); /* flags */
    krb5_store_stringz(sp, name);
    krb5_store_stringz(sp, inst);
    krb5_store_stringz(sp, realm);
    {
	unsigned char tmp[4] = { 0, 0, 0, 0 };
	int i;
	if(et->caddr){
	    for(i = 0; i < et->caddr->len; i++)
		if(et->caddr->val[i].addr_type == AF_INET &&
		   et->caddr->val[i].address.length == 4){
		    memcpy(tmp, et->caddr->val[i].address.data, 4);
		    break;
		}
	}
	sp->store(sp, tmp, sizeof(tmp));
    }

    if(et->key.keytype != KEYTYPE_DES || 
       et->key.keyvalue.length != 8)
	return -1;
    sp->store(sp, et->key.keyvalue.data, 8);
    
    {
	time_t start = et->starttime ? *et->starttime : et->authtime;
	krb5_store_int8(sp, krb_time_to_life(start, et->endtime));
	krb5_store_int32(sp, start);
    }

    krb5_store_stringz(sp, sname);
    krb5_store_stringz(sp, sinst);
    
    {
	krb5_data data;
	krb5_storage_to_data(sp, &data);
	krb5_storage_free(sp);
	*size = (data.length + 7) & ~7; /* pad to 8 bytes */
	if(*size > len)
	    return -1;
	memset(buf - *size + 1, 0, *size);
	memcpy(buf - *size + 1, data.data, data.length);
	krb5_data_free(&data);
    }
    return 0;
}

#endif
