#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>

#include "logfile.h"
#include "zzmalloc.h"
#include "utils.h"
#include "network.h"
#include "serial.h"
#include "synclog.h"
#include "common.h"
#include "dumpfile.h"
#include "sslave.h"

#define MAX_SYNC_PREV   100
#define SYNC_HEAD_LEN   (sizeof(int)+sizeof(int))

static int
sslave_forever(SSlave *ss)
{
	char recvbuf[1024];
	//int  recvsize = 0;
	int  checksize = SYNC_HEAD_LEN + sizeof(int);
	int  ret;
	unsigned int  rlen = 0;
	// send sync
	
	while (1) {
		ret = readn(ss->sock, recvbuf, checksize, 0);
		if (ret < checksize) {
			DERROR("read sync head too short: %d, close\n", ret);
			close(ss->sock);
			ss->sock = -1;
			return -1;
		}
		
		memcpy(&rlen, recvbuf + SYNC_HEAD_LEN, sizeof(int));
		
		ret = readn(ss->sock, recvbuf + checksize, rlen, 0);
		if (ret < rlen) {
			DERROR("read sync too short: %d, close\n", ret);
			close(ss->sock);
			ss->sock = -1;
			return -1;
		}

		unsigned int size = checksize + rlen;	
	
		ret = wdata_apply(recvbuf + SYNC_HEAD_LEN, rlen, 0);
		if (ret == 0) {
			synclog_write(g_runtime->synclog, recvbuf, size);
		}
	}

	return 0;
}

int
sslave_load_master_dump_info(SSlave *ss)
{
	char dumpfile[PATH_MAX];	
	int  ret;

	snprintf(dumpfile, PATH_MAX, "%s/dump.master.dat", g_cf->datadir);
	
	if (isfile(dumpfile)) {
		FILE	*dumpf;	
		dumpf = fopen(dumpfile, "r");
		if (dumpf == NULL) {
			DERROR("open file %s error! %s\n", dumpfile, strerror(errno));
			return -1;
		}
		
		fseek(dumpf, 0, SEEK_END);
		ss->dumpfile_size = ftell(dumpf);

		if (ss->dumpfile_size >= DUMP_HEAD_LEN) {
			long long size;
			fseek(dumpf, DUMP_HEAD_LEN - sizeof(long long), SEEK_SET);
			ret = fread(&size, sizeof(long long), 1, dumpf);
			if (ret != sizeof(long long)) {
				DERROR("fread error: %s\n", strerror(errno));
				fclose(dumpf);
				return -1;
				//goto read_dump_over;
			}
			ss->dumpsize = size;

			int pos = sizeof(short) + sizeof(int);
			fseek(dumpf, pos, SEEK_SET);

			unsigned int dump_logver;
			ret = fread(&dump_logver, sizeof(int), 1, dumpf);
			if (ret != sizeof(int)) {
				DERROR("fread error: %s\n", strerror(errno));
				fclose(dumpf);
				return -1;
				//goto read_dump_over;
			}
			ss->dump_logver = dump_logver;
		}
		fclose(dumpf);
	}

	return 0;
}

int
sslave_load_dump_info(SSlave *ss)
{
	char dumpfile[PATH_MAX];	
	int  ret;

	snprintf(dumpfile, PATH_MAX, "%s/dump.dat", g_cf->datadir);
	
	FILE	*dumpf;	
	dumpf = fopen(dumpfile, "r");
	if (dumpf == NULL) {
		DERROR("open file %s error! %s\n", dumpfile, strerror(errno));
		return -1;
	}

	int pos = sizeof(short) + sizeof(int);
	fseek(dumpf, pos, SEEK_SET);

	unsigned int dump_logver;
	ret = fread(&dump_logver, sizeof(int), 1, dumpf);
	if (ret != sizeof(int)) {
		DERROR("fread error: %s\n", strerror(errno));
		fclose(dumpf);
		return -1;
	}
	ss->binlog_min_ver = dump_logver;
	fclose(dumpf);

	return 0;
}

/* 
 *
 */
static int 
sslave_prev_sync_pos(SSlave *ss)
{
	char filepath[PATH_MAX];
	int bver = ss->binlog_ver;
	int ret;
    int i;

    for (i = 0; i < MAX_SYNC_PREV; i++) {
        if (ss->binlog_index == 0) {
            if (ss->binlog) {
                synclog_destroy(ss->binlog);
                ss->binlog = NULL;
            }
            ret = synclog_prevlog(ss->binlog_ver);		
            if (ret <= 0) {
                DERROR("synclog_prevlog error: %d\n", ret);
                return -1;
            }
            ss->binlog_ver   = ret;
            ss->binlog_index = 0;

            if (ss->binlog_ver > 0) {
                snprintf(filepath, PATH_MAX, "%s/bin.log.%d", g_cf->datadir, bver);
            }else{
                snprintf(filepath, PATH_MAX, "%s/bin.log", g_cf->datadir);
            }
            DINFO("try open synclog: %s\n", filepath);
            ss->binlog = synclog_open(filepath);
            if (NULL == ss->binlog) {
                DERROR("open synclog error: %s\n", filepath);
                return -1;
            }

            ss->binlog_index = ss->binlog->index_pos;
        }

        ss->binlog_index -= 1;
        int pos = ss->binlog_index - 1;

        lseek(ss->binlog->fd, pos, SEEK_SET);
        unsigned int logver, logline;
        ret = readn(ss->binlog->fd, &logver, sizeof(int), 0);
        if (ret != sizeof(int)) {
            DERROR("read logver error! %s\n", strerror(errno));
            continue;
            //return -1;
        }
        ret = readn(ss->binlog->fd, &logline, sizeof(int), 0);
        if (ret != sizeof(int)) {
            DERROR("read logline error! %s\n", strerror(errno));
            continue;
            //return -1;
        }

        ss->logver  = logver;
        ss->logline = logline;
        break;
    }

	return 0;
}


int 
sslave_check_sync(SSlave *ss)
{
	//char dumpfile[PATH_MAX];	
	//int  ret;

	sslave_load_master_dump_info(ss);
	sslave_load_dump_info(ss);

	char *binlog;
	char logname[PATH_MAX];

	snprintf(logname, PATH_MAX, "%s/bin.log", g_cf->datadir);

	binlog = logname;	
	FILE	*binf;

	binf = fopen(binlog, "r");
		
	

	fclose(binf);

	return 0;
}


int 
sslave_do_cmd(SSlave *ss, char *sendbuf, int buflen, char *recvbuf, int recvlen)
{
    int ret;

    ret = writen(ss->sock, sendbuf, buflen, ss->timeout);
    if (ret != buflen) {
        sslave_close(ss);
        DINFO("cmd writen error: %d, close.\n", ret);
        return -1;
    }
    
    //char recv[1024];
    ret = readn(ss->sock, recvbuf, sizeof(short), ss->timeout);
    if (ret != sizeof(short)) {
        sslave_close(ss);
        DINFO("cmd readn error: %d, close.\n", ret);
        return -2;
    }
    
    unsigned short len;
    
    memcpy(&len, recvbuf, sizeof(short));
    DINFO("cmd recv len:%d\n", len);
    
    if (len + sizeof(short) > recvlen) {
        sslave_close(ss);
        DERROR("recv data too long: %d\n", (int)(len + sizeof(short)));
        return -3;
    }

    ret = readn(ss->sock, recvbuf + sizeof(short), len, ss->timeout);
    if (ret != len) {
        sslave_close(ss);
        DINFO("cmd readn error: %d, close.\n", ret);
        return -4;
    }

    return len + sizeof(short);
}


int
sslave_conn_init(SSlave *ss)
{
    char sndbuf[1024];
    int  sndlen;
    char recvbuf[1024];
    //int  recvlen;
    int  ret;
    unsigned int  logver;
    unsigned int  logline;
    unsigned int  dumpver;
    long long     dumpsize;

    if (ss->dumpsize == ss->dumpfile_size) { // have master dumpfile, and size ok or not have master dumpfile
        // send sync command 
        while (1) {
            logver  = ss->logver;
            logline = ss->logline;
			DINFO("logver: %u, logline: %u\n", logver, logline);
            sndlen = cmd_sync_pack(sndbuf, logver, logline); 
            ret = sslave_do_cmd(ss, sndbuf, sndlen, recvbuf, 1024);
            if (ret < 0) {
                return ret;
            }

            char syncret; 
            int  i = sizeof(short);

            syncret = recvbuf[i];

            if (syncret == 0) // ok, recv synclog
                return 0;

            i += 1;
            memcpy(&dumpver, &recvbuf[i], sizeof(int));
            // try find the last sync position
            ret = sslave_prev_sync_pos(ss);
            if (ret != 0) { // no prev log, or get error, try get dump
                dumpsize = 0;
                break;
            }
        } 
    }else{
        dumpsize = ss->dumpfile_size;
    }
    
	// do getdump
    sndlen = cmd_getdump_pack(sndbuf, dumpver, dumpsize); 
    ret = sslave_do_cmd(ss, sndbuf, sndlen, recvbuf, 1024); 
    if (ret < 0) {
        DERROR("cmd getdump error: %d\n", ret);
        return ret;
    }

    long long size, rlen = 0;
    memcpy(&size, recvbuf + CMD_REPLY_HEAD_LEN, sizeof(long long));

    DINFO("dump len: %lld\n", size);

    char    dumpbuf[8192];
    int     buflen = 0;
    char    dumpfile[PATH_MAX];
    char    dumpfile_tmp[PATH_MAX];
    int     fd;

    snprintf(dumpfile, PATH_MAX, "%s/dump.master.dat", g_cf->datadir);
    snprintf(dumpfile_tmp, PATH_MAX, "%s/dump.master.dat.tmp", g_cf->datadir);
    
    fd = open(dumpfile_tmp, O_CREAT|O_WRONLY, 0644);
    if (fd == -1) {
        DERROR("open dumpfile %s error! %s\n", dumpfile_tmp, strerror(errno));
        return -2;
    }
    
    while (rlen < size) {
        ret = readn(ss->sock, dumpbuf, 8192, ss->timeout);    
        if (ret < 0) {
            DERROR("read dump error: %d\n", ret);
            close(fd);
            return ret;
        }
        buflen = ret;
        ret = writen(fd, dumpbuf, buflen, 0);
        if (ret < 0) {
            DERROR("write dump error: %d, %s\n", ret, strerror(errno));
            close(fd);
            return ret;
        }

        rlen += ret;
    }

    close(fd);
    
    ret = rename(dumpfile_tmp, dumpfile);
    if (ret == -1) {
        DERROR("dump file rename error: %s\n", strerror(errno));
        return -1;
    }

    // load dump file
	hashtable_clear_all(g_runtime->ht);
	loaddump(g_runtime->ht, dumpfile);

	/*char olddump[PATH_MAX];
    snprintf(olddumpfile, PATH_MAX, "%s/dump.dat", g_cf->datadir);
	remove();*/ 

	return 0;
}

int
sslave_recv_synclog(SSlave *ss)
{
    int  ret;
    int  len;
    char buf[1024];
    //char buflen = 0;
    int  headlen = SYNC_HEAD_LEN + sizeof(short);

    while (ss->sock) {
        ret = readn(ss->sock, buf, headlen, ss->timeout);         
        if (ret != headlen) {
            DERROR("readn synclog head error! %d, close\n", ret);
            sslave_close(ss);
            return -1;
        }

        memcpy(&len, buf + SYNC_HEAD_LEN, sizeof(short));
        DINFO("recv synclog len: %d\n", len);

        ret = readn(ss->sock, buf + headlen, len, ss->timeout);
        if (ret != len) {
            DERROR("readn synclog error! %d, close\n", ret);
            sslave_close(ss);
            return -2;
        }
       
        ret = wdata_apply(buf + SYNC_HEAD_LEN, len + sizeof(short), 0);
        if (MEMLINK_OK == ret) {
            synclog_write(g_runtime->synclog, buf, len + headlen);
        }else{
            DERROR("wdata_apply error: %d\n", ret);
            sslave_close(ss);
            return -3;
        }
    }

	return 0;
}


/**
 * slave sync thread
 * 1.find local sync logver/logline 
 * 2.send sync command to server 
 * 3.get dump/sync message
 */
void*
sslave_run(void *args)
{
	SSlave	*ss = (SSlave*)args;
	int		ret;

	while (1) {
		if (ss->sock <= 0) {
			ret = tcp_socket_connect(g_cf->master_sync_host, g_cf->master_sync_port, ss->timeout);
			if (ret <= 0) {
				DERROR("connect error! ret:%d, continue.\n", ret);
				sleep(1);
				continue;
			}

			// do sync, getdump
			ret = sslave_conn_init(ss);
			if (ret < 0) {
				continue;
			}
			break;
		}
		
		// recv sync log from master
		ret = sslave_forever(ss);
	}

	return NULL;
}

SSlave*
sslave_create() 
{
    SSlave *ss;

    ss = (SSlave*)zz_malloc(sizeof(SSlave));
    if (ss == NULL) {
        DERROR("sslave malloc error.\n");
        MEMLINK_EXIT;
    }
    memset(ss, 0, sizeof(SSlave));

	ss->timeout = g_cf->timeout;

    pthread_t threadid;
    pthread_attr_t attr;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret != 0) {
        DERROR("pthread_attr_init error: %s\n", strerror(errno));
        MEMLINK_EXIT;
    }

    ret = pthread_create(&threadid, &attr, sslave_run, ss);
    if (ret != 0) {
        DERROR("pthread_create error: %s\n", strerror(errno));
        MEMLINK_EXIT;
    }
    
    ret = pthread_detach(threadid);
    if (ret != 0) {
        DERROR("pthread_detach error; %s\n", strerror(errno));
        MEMLINK_EXIT;
    }

    DINFO("create sync slave thread ok!\n");

    return ss;
}

void
sslave_close(SSlave *ss)
{
    if (ss->sock) {
        close(ss->sock);
        ss->sock = -1;
    }
    
    ss->logver  = 0;
    ss->logline = 0;
}

void 
sslave_destroy(SSlave *ss) 
{
    if (ss->sock) {
        close(ss->sock);
        ss->sock = -1;
    }
    zz_free(ss);
}

