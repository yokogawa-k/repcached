/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *
 */
#include "memcached.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static Q_ITEM *q_freelist  = NULL;
static int     q_itemcount = 0;

int get_qi_count()
{
    return(q_itemcount);
}

Q_ITEM *qi_new(enum CMD_TYPE type, R_CMD *cmd, bool reuse)
{
    Q_ITEM     *q      = NULL;
    char       *key    = NULL;
    uint32_t    keylen = 0;
    rel_time_t  time   = 0;

    if(q_freelist){
        q = q_freelist;
        q_freelist = q->next;
    }

    if(NULL == q){
        if(reuse)
            return(NULL);
        if(q_itemcount >= settings.rep_qmax)
            return(NULL);
        q = malloc(sizeof(Q_ITEM));
        if (NULL == q){
            fprintf(stderr,"replication: qi_new out of memory\n");
            return(NULL);
        }
        q_itemcount++;
        if (settings.verbose > 2)
            fprintf(stderr,"replication: alloc c=%d\n", q_itemcount);
    }

    switch (type) {
    case REPLICATION_REP:
    case REPLICATION_DEL:
        key    = cmd->key;
        keylen = cmd->keylen;
        break;
    case REPLICATION_DEFER_DEL:
        key    = cmd->key;
        keylen = cmd->keylen;
        time   = cmd->time;
        break;
    case REPLICATION_FLUSH_ALL:
        break;
    case REPLICATION_DEFER_FLUSH_ALL:
        time   = cmd->time;
        break;
    case REPLICATION_MARUGOTO_END:
        break;
    default:
        fprintf(stderr,"replication: got unknown command: %d\n", type);
        return(NULL);
    }

    q->key  = NULL;
    q->type = type;
    q->time = time;
    q->next = NULL;
    if (keylen) {
        q->key = malloc(keylen + 1);
        if(NULL == q->key){
            qi_free(q);
            q = NULL;
        }else{
            memcpy(q->key, key, keylen);
            *(q->key + keylen) = 0;
        }
    }

    return(q);
}

void qi_free(Q_ITEM *q)
{
    if(q){
        if(q->key){
            free(q->key);
            q->key = NULL;
        }
        q->next = q_freelist;
        q_freelist = q;
    }
}

int qi_free_list()
{
    int     c = 0;
    Q_ITEM *q = NULL;

    while(q = q_freelist){
        q_itemcount--;
        c++;
        q_freelist = q->next;
        free(q);
    }
    return(c);
}

static int replication_get_num(char *p, int n)
{
    int  l;
    char b[64];
    if(p)
        l = sprintf(p, "%u", n);
    else
        l = sprintf(b, "%u", n);
    return(l);
}

int replication_call_rep(char *key, size_t keylen)
{
    R_CMD r;
    r.key    = key;
    r.keylen = keylen;
    return(replication(REPLICATION_REP, &r));
}

int replication_call_del(char *key, size_t keylen)
{
    R_CMD r;
    r.key    = key;
    r.keylen = keylen;
    return(replication(REPLICATION_DEL, &r));
}

int replication_call_defer_del(char *key, size_t keylen, rel_time_t time)
{
    R_CMD r;
    r.key    = key;
    r.keylen = keylen;
    r.time   = time;
    return(replication(REPLICATION_DEFER_DEL, &r));
}

int replication_call_flush_all()
{
    R_CMD r;
    r.key = NULL;
    return(replication(REPLICATION_FLUSH_ALL, &r));
}

int replication_call_defer_flush_all(const rel_time_t time)
{
    R_CMD r;
    r.key  = NULL;
    r.time = time;
    return(replication(REPLICATION_DEFER_FLUSH_ALL, &r));
}

int replication_call_marugoto_end()
{
    R_CMD r;
    r.key = NULL;
    return(replication(REPLICATION_MARUGOTO_END, &r));
}

static int replication_alloc(conn *c, int s)
{
    char *p;
    s += c->wbytes;
    s += c->wcurr - c->wbuf;
    if(c->wsize < s){
        while(c->wsize < s){
            c->wsize += DATA_BUFFER_SIZE;
        }
        if(c->wsize > MAX_SENDBUF_SIZE){
            fprintf(stderr, "replication: alloc error: wsize over MAX_SENDBUF_SIZE\n");
            return(-1);
        }
        if(!(p = malloc(c->wsize))){
            fprintf(stderr, "replication: alloc error: %s\n", strerror(errno));
            return(-1);
        }
        memcpy(p, c->wcurr, c->wbytes);
        free(c->wbuf);
        c->wbuf = p;
        c->wcurr = p;
    }
    return(0);
}

static int replication_del(conn *c, char *k)
{
    int   l = 0;
    char *s = "delete ";
    char *n = "\r\n";
    char *p = NULL;

    l += strlen(s);
    l += strlen(k);
    l += strlen(n);
    if(replication_alloc(c,l) == -1){
        fprintf(stderr, "replication: del alloc error\n");
        return(-1);
    }
    p = c->wcurr + c->wbytes;
    memcpy(p, s, strlen(s));
    p += strlen(s);
    memcpy(p, k, strlen(k));
    p += strlen(k);
    memcpy(p, n, strlen(n));
    p += strlen(n);
    c->wbytes = p - c->wcurr;
    return(0);
}

static int replication_defer_del(conn *c, char *k, rel_time_t exp)
{
    int   l = 0;
    char *s = "delete ";
    char *n = "\r\n";
    char *p = NULL;

    l += strlen(s);
    l += strlen(k);
    l += 1;
    l += replication_get_num(NULL, exp);
    l += strlen(n);
    if(replication_alloc(c,l) == -1){
        fprintf(stderr, "replication: defer del alloc error\n");
        return(-1);
    }
    p = c->wcurr + c->wbytes;
    memcpy(p, s, strlen(s));
    p += strlen(s);
    memcpy(p, k, strlen(k));
    p += strlen(k);
    *(p++) = ' ';
    p += replication_get_num(p, exp);
    memcpy(p, n, strlen(n));
    p += strlen(n);
    c->wbytes = p - c->wcurr;
    return(0);
}

static int replication_rep(conn *c, item *it)
{
    int   r = 0;
    int exp = 0;
    int len = 0;
    char *s = "rep ";
    char *n = "\r\n";
    char *p = NULL;
    char flag[40];

    if(it->exptime)
        exp = it->exptime + stats.started;
    flag[0]=0;
    if(p=ITEM_suffix(it)){
        int i;
        memcpy(flag, p, it->nsuffix - 2);
        flag[it->nsuffix - 2] = 0;
        for(i=0;i<strlen(flag);i++){
            if(flag[i] > ' ')
                break;
        }
        memmove(flag,&flag[i],strlen(flag)-i);
        for(p=flag;*p>' ';p++);
        *p=0;
    }
    len += strlen(s);
    len += it->nkey;
    len += 1;
    len += strlen(flag);
    len += 1;
    len += replication_get_num(NULL, exp);
    len += 1;
    len += replication_get_num(NULL, it->nbytes - 2);
    len += 1;
    len += replication_get_num(NULL, it->cas_id);
    len += strlen(n);
    len += it->nbytes;
    len += strlen(n);
    if(replication_alloc(c,len) == -1){
        fprintf(stderr, "replication: rep alloc error\n");
        return(-1);
    }
    p = c->wcurr + c->wbytes;
    memcpy(p, s, strlen(s));
    p += strlen(s);
    memcpy(p, ITEM_key(it), it->nkey);
    p += it->nkey;
    *(p++) = ' ';
    memcpy(p, flag, strlen(flag));
    p += strlen(flag);
    *(p++) = ' ';
    p += replication_get_num(p, exp);
    *(p++) = ' ';
    p += replication_get_num(p, it->nbytes - 2);
    *(p++) = ' ';
    p += replication_get_num(p, it->cas_id);
    memcpy(p, n, strlen(n));
    p += strlen(n);
    memcpy(p, ITEM_data(it), it->nbytes);
    p += it->nbytes;
    c->wbytes = p - c->wcurr;
    return(0);
}

static int replication_flush_all(conn *c, rel_time_t exp)
{
    char *s = "flush_all ";
    char *n = "\r\n";
    char *p = NULL;

    int l = strlen(s) + strlen(n);
    if (exp > 0)
        l += replication_get_num(NULL, exp);
    if(replication_alloc(c,l) == -1){
        fprintf(stderr, "replication: flush_all alloc error\n");
        return(-1);
    }
    p = c->wcurr + c->wbytes;
    memcpy(p, s, strlen(s));
    p += strlen(s);
    if (exp > 0)
        p += replication_get_num(p, exp);
    memcpy(p, n, strlen(n));
    p += strlen(n);
    c->wbytes = p - c->wcurr;
    return(0);
}

static int replication_marugoto_end(conn *c)
{
    char *s = "marugoto_end";
    char *n = "\r\n";
    char *p = NULL;

    int l = strlen(s) + strlen(n);
    if(replication_alloc(c,l) == -1){
        fprintf(stderr, "replication: marugoto_end alloc error\n");
        return(-1);
    }
    p = c->wcurr + c->wbytes;
    memcpy(p, s, strlen(s));
    p += strlen(s);
    memcpy(p, n, strlen(n));
    p += strlen(n);
    c->wbytes = p - c->wcurr;
    return(0);
}

int replication_cmd(conn *c, Q_ITEM *q)
{
    item *it;
    switch (q->type) {
    case REPLICATION_REP:
        if(it = assoc_find(q->key, strlen(q->key)))
            return(replication_rep(c, it));
        else
            return(replication_del(c, q->key));
    case REPLICATION_DEL:
        return(replication_del(c, q->key));
    case REPLICATION_DEFER_DEL:
        return(replication_defer_del(c, q->key, q->time));
    case REPLICATION_FLUSH_ALL:
        return(replication_flush_all(c, 0));
    case REPLICATION_DEFER_FLUSH_ALL:
        return(replication_flush_all(c, q->time));
    case REPLICATION_MARUGOTO_END:
        return(replication_marugoto_end(c));
    default:
        fprintf(stderr,"replication: got unknown command:%d\n", q->type);
        return(0);
    }
}
