#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <jansson.h>
#include <libb64/cencode.h>
#include "conf.h"

static struct acl *
conf_parse_acls(json_t *jtab);

struct conf *
conf_read(const char *filename) {

	json_t *j;
	json_error_t error;
	struct conf *conf;
	void *kv;

	/* defaults */
	conf = calloc(1, sizeof(struct conf));
	conf->redis_host = strdup("127.0.0.1");
	conf->redis_port = 6379;
	conf->http_host = strdup("0.0.0.0");
	conf->http_port = 7379;

	j = json_load_file(filename, 0, &error);
	if(!j) {
		fprintf(stderr, "Error: %s (line %d)\n", error.text, error.line);
		return conf;
	}

	for(kv = json_object_iter(j); kv; kv = json_object_iter_next(j, kv)) {
		json_t *jtmp = json_object_iter_value(kv);

		if(strcmp(json_object_iter_key(kv), "redis_host") == 0 && json_typeof(jtmp) == JSON_STRING) {
			free(conf->redis_host);
			conf->redis_host = strdup(json_string_value(jtmp));
		} else if(strcmp(json_object_iter_key(kv), "redis_port") == 0 && json_typeof(jtmp) == JSON_INTEGER) {
			conf->redis_port = (short)json_integer_value(jtmp);
		} else if(strcmp(json_object_iter_key(kv), "redis_auth") == 0 && json_typeof(jtmp) == JSON_STRING) {
			conf->redis_auth = strdup(json_string_value(jtmp));
		} else if(strcmp(json_object_iter_key(kv), "http_host") == 0 && json_typeof(jtmp) == JSON_STRING) {
			free(conf->http_host);
			conf->http_host = strdup(json_string_value(jtmp));
		} else if(strcmp(json_object_iter_key(kv), "http_port") == 0 && json_typeof(jtmp) == JSON_INTEGER) {
			conf->http_port = (short)json_integer_value(jtmp);
		} else if(strcmp(json_object_iter_key(kv), "acl") == 0 && json_typeof(jtmp) == JSON_ARRAY) {
			conf->perms = conf_parse_acls(jtmp);
		}
	}

	json_decref(j);

	return conf;
}

void
acl_read_commands(json_t *jlist, struct acl_commands *ac) {

	unsigned int i, n, cur;

	/* count strings in the array */
	for(i = 0, n = 0; i < json_array_size(jlist); ++i) {
		json_t *jelem = json_array_get(jlist, (size_t)i);
		if(json_typeof(jelem) == JSON_STRING) {
			n++;
		}
	}

	/* allocate block */
	ac->commands = calloc((size_t)n, sizeof(char*));
	ac->count = n;

	/* add all disabled commands */
	for(i = 0, cur = 0; i < json_array_size(jlist); ++i) {
		json_t *jelem = json_array_get(jlist, i);
		if(json_typeof(jelem) == JSON_STRING) {
			size_t sz;
			const char *s = json_string_value(jelem);
			sz = strlen(s);

			ac->commands[cur] = calloc(1 + sz, 1);
			memcpy(ac->commands[cur], s, sz);
			cur++;
		}
	}
}

struct acl *
conf_parse_acl(json_t *j) {

	json_t *jcidr, *jbasic, *jlist;
	unsigned short mask_bits = 0;

	struct acl *a = calloc(1, sizeof(struct acl));

	/* parse CIDR */
	if((jcidr = json_object_get(j, "ip")) && json_typeof(jcidr) == JSON_STRING) {
		const char *s;
		char *p, *ip;

		s = json_string_value(jcidr);
		p = strchr(s, '/');
		if(!p) {
			ip = strdup(s);
		} else {
			ip = calloc((size_t)(p - s + 1), 1);
			memcpy(ip, s, (size_t)(p - s));
			mask_bits = (unsigned short)atoi(p+1);
		}
		a->cidr.enabled = 1;
		a->cidr.mask = (mask_bits == 0 ? 0 : (0xffffffff << (32 - mask_bits)));
		a->cidr.subnet = ntohl(inet_addr(ip)) & a->cidr.mask;
		free(ip);
	}

	/* parse basic_auth */
	if((jbasic = json_object_get(j, "http_basic_auth")) && json_typeof(jbasic) == JSON_STRING) {

		/* base64 encode */
		base64_encodestate b64;
		int pos;
		char *p;
		const char *plain = json_string_value(jbasic);
		size_t len, plain_len = strlen(plain) + 0;
		len = (plain_len + 8) * 8 / 6;
		a->http_basic_auth = calloc(len, 1);
		
		base64_init_encodestate(&b64);
		pos = base64_encode_block(plain, (int)plain_len, a->http_basic_auth, &b64); /* FIXME: check return value */
		base64_encode_blockend(a->http_basic_auth + pos, &b64);

		/* end string with \0 rather than \n */
		if((p = strchr(a->http_basic_auth + pos, '\n'))) {
			*p = 0;
		}
	}

	/* parse enabled commands */
	if((jlist = json_object_get(j, "enabled")) && json_typeof(jlist) == JSON_ARRAY) {
		acl_read_commands(jlist, &a->enabled);
	}

	/* parse disabled commands */
	if((jlist = json_object_get(j, "disabled")) && json_typeof(jlist) == JSON_ARRAY) {
		acl_read_commands(jlist, &a->disabled);
	}

	return a;
}

struct acl *
conf_parse_acls(json_t *jtab) {

	struct acl *head = NULL, *tail = NULL, *tmp;

	unsigned int i;
	for(i = 0; i < json_array_size(jtab); ++i) {
		json_t *val = json_array_get(jtab, i);

		tmp = conf_parse_acl(val);
		if(head == NULL && tail == NULL) {
			head = tail = tmp;
		} else {
			tail->next = tmp;
			tail = tmp;
		}
	}

	return head;
}

int
acl_match(struct acl *a, in_addr_t *ip) {

	/* TODO: add HTTP Basic Auth */

	if(a->cidr.enabled == 0) { /* none given, all match */
		return 1;
	}

	/* CIDR check. */
	if(((*ip) & a->cidr.mask) == (a->cidr.subnet & a->cidr.mask)) {
		return 1;
	}

	return 0;
}


void
conf_free(struct conf *conf) {

	free(conf->redis_host);
	free(conf->redis_auth);

	free(conf->http_host);

	free(conf);
}
