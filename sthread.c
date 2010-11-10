#include <stdlib.h>
#include <network.h>
#include <string.h>
#include <errno.h>
#include <dirent.h> 
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "sthread.h"
#include "wthread.h"
#include "logfile.h"
#include "myconfig.h"
#include "zzmalloc.h"
#include "utils.h"
#include "common.h"

#define BUF_SIZE 1024

/**
 * Connection for sending sync log.
 */
typedef struct _syncConn 
{
    int sock; // socket file descriptor

    int log_fd; // sync log file descriptor
    char log_name[PATH_MAX];
    unsigned int log_ver; // sync log version
    unsigned int log_index_pos; // sync log index position

    int dump_fd;

    struct event evt;
    struct event_base *base;
} SyncLogConn;

typedef struct _dumpConn
{
    int sock;
    int dump_fd;

    struct event evt;
    struct event_base *base;
} DumpConn;

/**
 * Malloc. Terminate program if fails.
 */
static void* 
sthread_malloc(size_t size)
{
    void *ptr = zz_malloc(size);
    if (ptr == NULL) {
        DERROR("malloc error\n");
        MEMLINK_EXIT;
    }
    return ptr;
}

static SyncLogConn*
synclog_conn_create() 
{
    SyncLogConn* conn = (SyncLogConn*) sthread_malloc(sizeof(SyncLogConn));
    memset(conn, 0, sizeof(SyncLogConn));
    return conn;
}

/*
 *static void 
 *synclog_conn_destroy(SyncLogConn *conn) 
 *{
 *    event_del(&conn->evt);
 *    close(conn->sock);
 *    close(conn->log_fd);
 *    zz_free(conn);
 *}
 */

static DumpConn*
dump_conn_create() 
{
    DumpConn* conn = (DumpConn*) sthread_malloc(sizeof(DumpConn));
    memset(conn, 0, sizeof(DumpConn));
    return conn;
}

static void
dump_conn_destroy(DumpConn *conn) 
{
    event_del(&conn->evt);
    close(conn->dump_fd);
    zz_free(conn);
}

static void*
sthread_loop(void *arg) 
{
    SThread *st = (SThread*) arg;
    DINFO("sthread_loop...\n");
    event_base_loop(st->base, 0);
    return NULL;
}

static SThread* 
create_thread(SThread* st) 
{
    pthread_t threadid;
    pthread_attr_t attr;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret != 0) {
        DERROR("pthread_attr_init error: %s\n", strerror(errno));
        MEMLINK_EXIT;
    }
    ret = pthread_create(&threadid, &attr, sthread_loop, st);
    if (ret != 0) {
        DERROR("pthread_create error: %s\n", strerror(errno));
        MEMLINK_EXIT;
    }
    DINFO("create sync thread ok\n");

    return st;
}

/**
 * Return the synclog file pathname for the given synclog version.
 * 
 * @param log_ver synclog version
 * @param log_name return the synclog file pathname
 */
static void 
get_synclog_filename(unsigned int log_ver, char *log_name)
{
    // TODO race condition?
    if (log_ver != g_runtime->logver) 
        snprintf(log_name, PATH_MAX, "%s/data/bin.log.%d", g_runtime->home, log_ver);
    else 
        snprintf(log_name, PATH_MAX, "%s/data/bin.log", g_runtime->home);
}

/**
 * Open the synclog for the given synclog version.
 *
 * @param log_ver synclog version
 * @param log_name return the synclog file pathname
 * @param log_fd_ptr return the opened file descriptor
 */
static void 
open_synclog(unsigned int log_ver, char *log_name, int *log_fd_ptr) 
{
    get_synclog_filename(log_ver, log_name);
    *log_fd_ptr = open(log_name, O_RDONLY);
    if (*log_fd_ptr == -1) {
        DERROR("open file %s error! %s\n", log_name, strerror(errno));
        MEMLINK_EXIT;
    }
}

/**
 * Return the log index position for the given log record number.
 * 
 * @param log_no log record number
 * @return log record index position
 */
inline static unsigned int
get_index_pos(unsigned int log_no)
{
    return sizeof(short) + 
        sizeof(int) + 
        sizeof(char) + 
        sizeof(int) + 
        log_no * sizeof(int);
}

/**
 * Seek to the given offset and read an int.
 */
static unsigned int
seek_and_read_int(int fd, unsigned int offset)
{
    unsigned int integer;
    // seek
    if (lseek(fd, offset, SEEK_SET) == -1) {
        DERROR("lseek error! %s\n", strerror(errno));
        close(fd);
        MEMLINK_EXIT;    
    }
    // read
    if (read(fd, &integer, sizeof(int)) == -1) {
        DERROR("read error! %s\n", strerror(errno));
        close(fd);
        MEMLINK_EXIT;    
    }
    return integer;
}

/**
 * Return synclog record position. A 0 value of record position means that the 
 * record does not exist.
 */
static int
get_synclog_record_pos(unsigned int log_ver, unsigned int log_no)
{
    int log_fd;
    char log_name[PATH_MAX];
    open_synclog(log_ver, log_name, &log_fd);

    unsigned int log_index_pos = get_index_pos(log_no);
    unsigned int log_pos = seek_and_read_int(log_fd, log_index_pos);
    if (log_pos > 0) {
        DINFO("log record %d is at %d\n", log_no, log_pos);
    } else {
        DERROR("log record %d does not exist in log file %s\n", log_no, log_name);
    }
    return log_pos;
}

static struct timeval
create_interval(unsigned int seconds) 
{
    struct timeval tv;
    evutil_timerclear(&tv);
    tv.tv_sec = seconds;
    return tv;
}

/**
 * Read n bytes. Terminate program if error.
 */
static int
sthread_readn(int fd, void *ptr, size_t n)
{
    int ret;
    if ((ret = readn(fd, ptr, n, 0)) < 0) 
        MEMLINK_EXIT;
    else
        return ret;
}

/**
 * Write n bytes with a timeout. Terminate program if writing error or n bytes 
 * can't all be written. 
 */
static void 
sthread_writen(int fd, void *ptr, size_t n)
{
    int ret;
    if ((ret = writen(fd, ptr, n, g_cf->timeout)) != n) {
        DERROR("Unable to write %d byes, only %d bytes are written.\n", n, ret);
        MEMLINK_EXIT;
    }
}

/**
 * Read n bytes from the synclog file and write them to sync connection socket.
 */
static void 
transfer(SyncLogConn *conn,  void *ptr, size_t n)
{
    int ret;
    if ((ret = sthread_readn(conn->log_fd, ptr, n)) != n) {
        DERROR("Unable to read %d bytes, only %d bytes are read.\n", n, ret);
        MEMLINK_EXIT;
    }
    sthread_writen(conn->sock, ptr, n);
}

/**
 * Send available synclog records in the current open synclog file in 
 * SyncLogConn.
 */
static void
send_synclog_record(SyncLogConn *conn)
{
    unsigned int log_pos;
    unsigned int integer;
    unsigned short len;
    char *buf;

    while ((log_pos = seek_and_read_int(conn->log_fd, conn->log_index_pos)) > 0) {
        transfer(conn, &integer, sizeof(int)); // log version
        transfer(conn, &integer, sizeof(int)); // log position 
        transfer(conn, &len, sizeof(short)); // log record length

        // log record
        buf = sthread_malloc(len);
        transfer(conn, buf, len);
        zz_free(buf);

        conn->log_index_pos += sizeof(int);
    }
}

static void
send_synclog(int fd, short event, void *arg) 
{
    SyncLogConn *conn = arg;
    /*
     * Send history synclog files and open the current synclog file.
     */
    while (conn->log_ver < g_runtime->logver) {
        send_synclog_record(conn);
        (conn->log_ver)++;
        open_synclog(conn->log_ver, conn->log_name, &conn->log_fd);
    }
    /*
     * Send available records in the current synclog file
     */
    send_synclog_record(conn);

    struct timeval interval = create_interval(g_cf->timeout);
    event_add(&conn->evt, &interval);
}

static void 
send_synclog_init(int sock, unsigned int log_ver, unsigned int log_no) 
{
    SyncLogConn* conn = synclog_conn_create();
    conn->sock = sock;
    conn->log_ver = log_ver;
    conn->log_index_pos = get_index_pos(log_no);
    open_synclog(conn->log_ver, conn->log_name, &conn->log_fd);

    evtimer_set(&conn->evt, send_synclog, conn);
    event_base_set(g_runtime->sthread->base, &conn->evt);
    struct timeval interval = create_interval(0); 
    event_add(&conn->evt, &interval);
}

static int
cmd_sync(Conn* conn, char *data, int datalen) 
{
    int ret;
    unsigned int log_ver;
    unsigned int log_no;
    cmd_sync_unpack(data, &log_ver, &log_no);
    DINFO("log version: %d, log record number: %d\n", log_ver, log_no);
    if ((get_synclog_record_pos(log_ver, log_no)) > 0) {
        ret = data_reply(conn, 0, NULL, 0);
        DINFO("Found sync log file (version = %d)\n", log_ver);
        send_synclog_init(conn->sock, log_ver, log_no);
    } else { 
        ret = data_reply(conn, 1, (char *)&(g_runtime->dumpver), sizeof(int));
        DINFO("Not found syn log file with version %d\n", log_ver);
    }
    return ret;
}

static int 
open_dump()
{
    char dump_filename[PATH_MAX];
    snprintf(dump_filename, PATH_MAX, "%s/dump.dat", g_cf->datadir);
    
    int fd;
    if ((fd = open(dump_filename, O_RDONLY)) == -1) {
        DERROR("open %s error! %s\n", dump_filename, strerror(errno));
        MEMLINK_EXIT;
    }
    return fd;
}

static long long
get_file_size(int fd)
{
   long long file_size;
   if ((file_size = lseek(fd, 0, SEEK_END)) == -1) {
       DERROR("lseek error! %s", strerror(errno));
       MEMLINK_EXIT;
   }
   return file_size;
}


static void 
send_dump(int fd, short event, void *arg)
{
    DumpConn* conn = arg;
    char buf[BUF_SIZE];
    int ret;
    /*
     * The current log version may be changed if dump event happends. So it is 
     * saved here.
     */
    unsigned int log_ver = g_runtime->logver;

    while ((ret = sthread_readn(conn->dump_fd, buf, BUF_SIZE)) > 0) 
        sthread_writen(conn->sock, buf, ret);
    dump_conn_destroy(conn);

    send_synclog_init(conn->sock, log_ver, 0); 
}

static void 
send_dump_init(int sock, int dump_fd)
{
    DumpConn* conn = dump_conn_create();
    conn->sock = sock;
    conn->dump_fd = dump_fd;

    evtimer_set(&conn->evt, send_dump, conn);
    event_base_set(g_runtime->sthread->base, &conn->evt);
    struct timeval interval = create_interval(0); 
    event_add(&conn->evt, &interval);
}

static int
cmd_get_dump(Conn* conn, char *data, int datalen)
{
    int ret;
    unsigned int dumpver;
    unsigned int size;
    int retcode;

    cmd_getdump_unpack(data, &dumpver, &size);
    DINFO("dump version: %d, synchronized data size: %d\n", dumpver, size);
    retcode = g_runtime->dumpver == dumpver ? 1 : 0;

    int dump_fd = open_dump(); 
    long long file_size = get_file_size(dump_fd);
    int retlen = sizeof(int) + sizeof(long long);
    char retrc[retlen];
    memcpy(retrc, &g_runtime->dumpver, sizeof(int));
    memcpy(retrc, &file_size, sizeof(int long));
    ret = data_reply(conn, retcode, retrc, retlen);

    send_dump_init(conn->sock, dump_fd);

    return ret;
}

static void 
sthread_read(int fd, short event, void *arg) 
{
    SThread *st = (SThread*) arg;
    Conn *conn;
    int ret;

    DINFO("sthread_read...\n");
    conn = conn_create(fd);

    if (conn) {
        conn->port = g_cf->sync_port;
        conn->base = st->base;

        DINFO("new conn: %d\n", conn->sock);
        DINFO("change event to read.");
        ret = change_event(conn, EV_READ | EV_PERSIST, 1);
        if (ret < 0) {
            DERROR("change_event error: %d, close conn.\n", ret);
            conn_destroy(conn);
        }
    }
}

SThread*
sthread_create() 
{
    SThread *st = (SThread*)zz_malloc(sizeof(WThread));
    if (st == NULL) {
        DERROR("sthread malloc error.\n");
        MEMLINK_EXIT;
    }
    memset(st, 0, sizeof(SThread));

    st->sock = tcp_socket_server(g_cf->sync_port);
    if (st->sock == -1) 
        MEMLINK_EXIT;
    DINFO("sync thread socket creation ok!\n");

    st->base = event_base_new();
    event_set(&st->event, st->sock, EV_READ | EV_PERSIST, sthread_read, st);
    event_base_set(st->base, &st->event);
    event_add(&st->event, 0);

    g_runtime->sthread = st;

    return create_thread(st);
}

int
sdata_ready(Conn *conn, char *data, int datalen) 
{
    int ret;
    char cmd;

    memcpy(&cmd, data + sizeof(short), sizeof(char));
    char buf[256] = {0};
    DINFO("data ready cmd: %d, data: %s\n", cmd, formath(data, datalen, buf, 256));
    switch (cmd) {
        case CMD_SYNC:
            ret = cmd_sync(conn, data, datalen);
            break;
        case CMD_GETDUMP:
            ret = cmd_get_dump(conn, data, datalen);
            break;
        default:
            ret = MEMLINK_ERR_CLIENT_CMD;
            break;
    } 
    DINFO("data_reply return: %d\n", ret);
    return 0;

    return 0;
}

void sthread_destroy(SThread *st) 
{
    zz_free(st);
}