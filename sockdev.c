/* SOCKDEV.C    (c) Copyright Hercules development, 2003-2006        */
/*              Socketdevice support                                 */

#include "hstdinc.h"

#include "hercules.h"

#include "opcode.h"


#if defined(WIN32) && defined(OPTION_DYNAMIC_LOAD) && !defined(HDL_USE_LIBTOOL) && !defined(_MSVC_)
  extern SYSBLK *psysblk;
  #define sysblk (*psysblk)
#endif


/*===================================================================*/
/*              S o c k e t  D e v i c e s ...                       */
/*===================================================================*/

// #define DEBUG_SOCKDEV

#ifdef DEBUG_SOCKDEV
    #define logdebug    logmsg
#else
    #define logdebug    1 ? ((void)0) : logmsg
#endif

/*-------------------------------------------------------------------*/
/* Working storage                                                   */
/*-------------------------------------------------------------------*/

static int init_done = FALSE;

static LIST_ENTRY  bind_head;      /* (bind_struct list anchor) */
static LOCK        bind_lock;      /* (lock for accessing list) */

/*-------------------------------------------------------------------*/
/* Initialization / termination functions...                         */
/*-------------------------------------------------------------------*/

static void init_sockdev ( void  );
static void term_sockdev ( void* );

static void init_sockdev ( void )
{
    if (init_done) return;
    InitializeListHead( &bind_head );
    initialize_lock( &bind_lock );
    hdl_adsc( "term_sockdev", term_sockdev, NULL );
    init_done = TRUE;
}

static void term_sockdev ( void* arg )
{
    UNREFERENCED( arg );
    if (!init_done) init_sockdev();
    SIGNAL_SOCKDEV_THREAD();
    join_thread   ( sysblk.socktid, NULL );
    detach_thread ( sysblk.socktid );
}

/*-------------------------------------------------------------------*/
/* unix_socket   create and bind a Unix domain socket                */
/*-------------------------------------------------------------------*/

int unix_socket (char* path)
{
#if !defined( HAVE_SYS_UN_H )
    UNREFERENCED(path);
    logmsg (_("HHCSD024E This build does not support Unix domain sockets.\n") );
    return -1;
#else // defined( HAVE_SYS_UN_H )

    struct sockaddr_un addr;
    int sd;

    logdebug ("unix_socket(%s)\n", path);

    if (strlen (path) > sizeof(addr.sun_path) - 1)
    {
        logmsg (_("HHCSD008E Socket pathname \"%s\" exceeds limit of %d\n"),
            path, (int) sizeof(addr.sun_path) - 1);
        return -1;
    }

    addr.sun_family = AF_UNIX;
    strcpy (addr.sun_path, path); /* guaranteed room by above check */
    sd = socket (PF_UNIX, SOCK_STREAM, 0);

    if (sd == -1)
    {
        logmsg (_("HHCSD009E Error creating socket for %s: %s\n"),
            path, strerror(HSO_errno));
        return -1;
    }

    unlink (path);
    fchmod (sd, 0700);

    if (0
        || bind (sd, (struct sockaddr*) &addr, sizeof(addr)) == -1
        || listen (sd, 0) == -1
        )
    {
        logmsg (_("HHCSD010E Failed to bind or listen on socket %s: %s\n"),
            path, strerror(HSO_errno));
        return -1;
    }

    return sd;

#endif // !defined( HAVE_SYS_UN_H )
}


/*-------------------------------------------------------------------*/
/* inet_socket   create and bind a regular TCP/IP socket             */
/*-------------------------------------------------------------------*/
int inet_socket (char* spec)
{
    /* We need a copy of the path to overwrite a ':' with '\0' */

    char buf[sizeof(((DEVBLK*)0)->filename)];
    char* colon;
    char* node;
    char* service;
    int sd;
    int one = 1;
    struct sockaddr_in sin;

    logdebug("inet_socket(%s)\n", spec);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    strcpy(buf, spec);
    colon = strchr(buf, ':');

    if (colon)
    {
        *colon = '\0';
        node = buf;
        service = colon + 1;
    }
    else
    {
        node = NULL;
        service = buf;
    }

    if (!node)
        sin.sin_addr.s_addr = INADDR_ANY;
    else
    {
        struct hostent* he = gethostbyname(node);

        if (!he)
        {
            logmsg (_("HHCSD011E Failed to determine IP address from %s\n"),
                node);
            return -1;
        }

        memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));
    }

    if (isdigit(service[0]))
    {
        sin.sin_port = htons(atoi(service));
    }
    else
    {
        struct servent* se = getservbyname(service, "tcp");

        if (!se)
        {
            logmsg (_("HHCSD012E Failed to determine port number from %s\n"),
                service);
            return -1;
        }

        sin.sin_port = se->s_port;
    }

    sd = socket (PF_INET, SOCK_STREAM, 0);

    if (sd == -1)
    {
        logmsg (_("HHCSD013E Error creating socket for %s: %s\n"),
            spec, strerror(HSO_errno));
        return -1;
    }

    setsockopt (sd, SOL_SOCKET, SO_REUSEADDR, (GETSET_SOCKOPT_T*)&one, sizeof(one));

    if (0
        || bind (sd, (struct sockaddr*) &sin, sizeof(sin)) == -1
        || listen (sd, 0) == -1
        )
    {
        logmsg (_("HHCSD014E Failed to bind or listen on socket %s: %s\n"),
            spec, strerror(HSO_errno));
        return -1;
    }

    return sd;
}


/*-------------------------------------------------------------------*/
/* add_socket_devices_to_fd_set   add all bound socket devices'      */
/*                                listening sockets to the FD_SET    */
/*-------------------------------------------------------------------*/
int add_socket_devices_to_fd_set (int maxfd, fd_set* readset)
{
    DEVBLK* dev;
    bind_struct* bs;
    LIST_ENTRY*  pListEntry;

    obtain_lock(&bind_lock);

    pListEntry = bind_head.Flink;

    while (pListEntry != &bind_head)
    {
        bs = CONTAINING_RECORD(pListEntry,bind_struct,bind_link);

        if (bs->sd != -1)           /* if listening for connections, */
        {
            dev = bs->dev;

            FD_SET(bs->sd, readset);    /* then add file to set */

            if (bs->sd > maxfd)
                maxfd = bs->sd;
        }

        pListEntry = pListEntry->Flink;
    }

    release_lock(&bind_lock);

    return maxfd;
}


/*-------------------------------------------------------------------*/
/* socket_device_connection_handler                                  */
/*-------------------------------------------------------------------*/
void socket_device_connection_handler (bind_struct* bs)
{
    struct sockaddr_in  client;         /* Client address structure  */
    struct hostent*     pHE;            /* Addr of hostent structure */
    socklen_t           namelen;        /* Length of client structure*/
    char*               clientip;       /* Addr of client ip address */
    char*               clientname;     /* Addr of client hostname   */
    DEVBLK*             dev;            /* Device Block pointer      */
    int                 csock;          /* Client socket             */

    dev = bs->dev;

    logdebug("socket_device_connection_handler(dev=%4.4X)\n",
        dev->devnum);

    /* Obtain the device lock */

    obtain_lock (&dev->lock);

    /* Reject if device is busy or interrupt pending */

    if (dev->busy || IOPENDING(dev)
     || (dev->scsw.flag3 & SCSW3_SC_PEND))
    {
        release_lock (&dev->lock);
        logmsg (_("HHCSD015E Connect to device %4.4X (%s) rejected; "
            "device busy or interrupt pending\n"),
            dev->devnum, bs->spec);
        return;
    }

    /* Reject if previous connection not closed (should not occur) */

    if (dev->fd != -1)
    {
        release_lock (&dev->lock);
        logmsg (_("HHCSD016E Connect to device %4.4X (%s) rejected; "
            "client %s (%s) still connected\n"),
            dev->devnum, bs->spec, bs->clientip, bs->clientname);
        close_socket(accept(bs->sd, 0, 0));  /* Should reject */
        return;
    }

    /* Accept the connection... */

    csock = accept(bs->sd, 0, 0);

    if (csock == -1)
    {
        release_lock (&dev->lock);
        logmsg (_("HHCSD017E Connect to device %4.4X (%s) failed: %s\n"),
            dev->devnum, bs->spec, strerror(HSO_errno));
        return;
    }

    /* Determine the connected client's IP address and hostname */

    namelen = sizeof(client);
    clientip = NULL;
    clientname = "host name unknown";

    if (1
        && getpeername(csock, (struct sockaddr*) &client, &namelen) == 0
        && (clientip = inet_ntoa(client.sin_addr)) != NULL
        && (pHE = gethostbyaddr((unsigned char*)(&client.sin_addr),
            sizeof(client.sin_addr), AF_INET)) != NULL
        && pHE->h_name && *pHE->h_name
        )
    {
        clientname = (char*) pHE->h_name;
    }

    /* Log the connection */

    if (clientip)
    {
        logmsg (_("HHCSD018I %s (%s) connected to device %4.4X (%s)\n"),
            clientip, clientname, dev->devnum, bs->spec);
    }
    else
    {
        logmsg (_("HHCSD019I <unknown> connected to device %4.4X (%s)\n"),
            dev->devnum, bs->spec);
    }

    /* Save the connected client information in the bind_struct */

    if (bs->clientip)   free(bs->clientip);
    if (bs->clientname) free(bs->clientname);

    bs->clientip   = strdup(clientip);
    bs->clientname = strdup(clientname);

    /* Indicate that a client is now connected to device (prevents
     * listening for new connections until THIS client disconnects).
     */

    dev->fd = csock;        /* (indicate client connected to device) */

    /* Release the device lock */

    release_lock (&dev->lock);

    /* Raise unsolicited device end interrupt for the device */

    device_attention (dev, CSW_DE);
}


/*-------------------------------------------------------------------*/
/* check_socket_devices_for_connections                              */
/*-------------------------------------------------------------------*/
void check_socket_devices_for_connections (fd_set* readset)
{
    bind_struct* bs;
    LIST_ENTRY*  pListEntry;

    obtain_lock(&bind_lock);

    pListEntry = bind_head.Flink;

    while (pListEntry != &bind_head)
    {
        bs = CONTAINING_RECORD(pListEntry,bind_struct,bind_link);

        if (bs->sd != -1 && FD_ISSET(bs->sd, readset))
        {
            /* Note: there may be other connection requests
             * waiting to be serviced, but we'll catch them
             * the next time the panel thread calls us. */

            release_lock(&bind_lock);
            socket_device_connection_handler(bs);
            return;
        }

        pListEntry = pListEntry->Flink;
    }

    release_lock(&bind_lock);
}

/*-------------------------------------------------------------------*/
/*    socket_thread       listen for socket device connections       */
/*-------------------------------------------------------------------*/
void* socket_thread( void* arg )
{
    int     rc;
    fd_set  sockset;
    int     maxfd = 0;
    int     select_errno;
    int     exit_now;

    UNREFERENCED( arg );

    SET_THREAD_NAME(-1,"socket_thread");

    /* Display thread started message on control panel */
    logmsg (_("HHCSD020I Socketdevice listener thread started: "
            "tid="TIDPAT", pid=%d\n"),
            thread_id(), getpid());

    for (;;)
    {
        /* Set the file descriptors for select */
        FD_ZERO ( &sockset );
        maxfd  = add_socket_devices_to_fd_set (   0,   &sockset );
        SUPPORT_WAKEUP_SOCKDEV_SELECT_VIA_PIPE( maxfd, &sockset );

        /* Do the select and save results */
        rc = select ( maxfd+1, &sockset, NULL, NULL, NULL );
        select_errno = HSO_errno;

        /* Clear the pipe signal if necessary */
        RECV_SOCKDEV_THREAD_PIPE_SIGNAL();

        /* Check if it's time to exit yet */
        obtain_lock( &bind_lock );
        exit_now = ( sysblk.shutdown || IsListEmpty( &bind_head ) );
        release_lock( &bind_lock );
        if ( exit_now ) break;

        /* Log select errors */
        if ( rc < 0 )
        {
            if ( HSO_EINTR != select_errno )
                logmsg( _( "HHCSD021E select failed; errno=%d: %s\n"),
                    select_errno, strerror( select_errno ) );
            continue;
        }

        /* Check if any sockets have received new connections */
        check_socket_devices_for_connections( &sockset );
    }

    logmsg( _( "HHCSD022I Socketdevice listener thread terminated\n" ) );

    return NULL;
}
/* (end socket_thread) */


/*-------------------------------------------------------------------*/
/* bind_device   bind a device to a socket (adds entry to our list   */
/*               of bound devices) (1=success, 0=failure)            */
/*-------------------------------------------------------------------*/
int bind_device (DEVBLK* dev, char* spec)
{
    bind_struct* bs;
    int was_list_empty;

    if (!init_done) init_sockdev();

    if (sysblk.shutdown) return 0;

    logdebug("bind_device (%4.4X, %s)\n", dev->devnum, spec);

    /* Error if device already bound */
    if (dev->bs)
    {
        logmsg (_("HHCSD001E Device %4.4X already bound to socket %s\n"),
            dev->devnum, dev->bs->spec);
        return 0;   /* (failure) */
    }

    /* Create a new bind_struct entry */
    bs = malloc(sizeof(bind_struct));
    if (!bs)
    {
        logmsg (_("HHCSD002E bind_device malloc() failed for device %4.4X\n"),
            dev->devnum);
        return 0;   /* (failure) */
    }
    memset(bs,0,sizeof(bind_struct));

    if (!(bs->spec = strdup(spec)))
    {
        logmsg (_("HHCSD003E bind_device strdup() failed for device %4.4X\n"),
            dev->devnum);
        free (bs);
        return 0;   /* (failure) */
    }

    /* Create a listening socket */
    if (bs->spec[0] == '/') bs->sd = unix_socket (bs->spec);
    else                    bs->sd = inet_socket (bs->spec);
    if (bs->sd == -1)
    {
        /* (error message already issued) */
        free( bs->spec );
        free( bs );
        return 0; /* (failure) */
    }

    /* Chain device and socket to each other */
    dev->bs = bs;
    bs->dev = dev;

    /* Add the new entry to our list of bound devices
       and create the socket thread that will listen
       for connections (if it doesn't already exist) */

    obtain_lock( &bind_lock );

    was_list_empty = IsListEmpty( &bind_head );

    InsertListTail( &bind_head, &bs->bind_link );

    if ( was_list_empty )
    {
        ATTR joinable_attr;
        initialize_join_attr( &joinable_attr );
        if ( create_thread( &sysblk.socktid, &joinable_attr,
                                    socket_thread, NULL ) )
            {
                logmsg( _( "HHCSD023E Cannot create socketdevice thread: errno=%d: %s\n" ),
                        errno, strerror( errno ) );
                RemoveListEntry( &bs->bind_link );
                close_socket(bs->sd);
                free( bs->spec );
                free( bs );
                release_lock( &bind_lock );
                return 0; /* (failure) */
            }
    }

    SIGNAL_SOCKDEV_THREAD();

    release_lock( &bind_lock );

    logmsg (_("HHCSD004I Device %4.4X bound to socket %s\n"),
        dev->devnum, dev->bs->spec);

    return 1;   /* (success) */
}


/*-------------------------------------------------------------------*/
/* unbind_device   unbind a device from a socket (removes entry from */
/*                 our list and discards it) (1=success, 0=failure)  */
/*-------------------------------------------------------------------*/
int unbind_device (DEVBLK* dev)
{
    bind_struct* bs;

    logdebug("unbind_device(%4.4X)\n", dev->devnum);

    /* Error if device not bound */
    if (!(bs = dev->bs))
    {
        logmsg (_("HHCSD005E Device %4.4X not bound to any socket\n"),
            dev->devnum);
        return 0;   /* (failure) */
    }

    /* Error if someone still connected */
    if (dev->fd != -1)
    {
        logmsg (_("HHCSD006E Client %s (%s) still connected to device %4.4X (%s)\n"),
            dev->bs->clientip, dev->bs->clientname, dev->devnum, dev->bs->spec);
        return 0;   /* (failure) */
    }

    /* Remove the entry from our list */

    obtain_lock( &bind_lock );
    RemoveListEntry( &bs->bind_link );
    SIGNAL_SOCKDEV_THREAD();
    release_lock( &bind_lock );

    logmsg (_("HHCSD007I Device %4.4X unbound from socket %s\n"),
        dev->devnum, bs->spec);

    if (bs->sd != -1)
        close_socket (bs->sd);

    /* Unchain device and socket from each another */

    dev->bs = NULL;
    bs->dev = NULL;

    /* Discard the entry */

    if ( bs->clientname ) free( bs->clientname );
    if ( bs->clientip   ) free( bs->clientip   );

    bs->clientname = NULL;
    bs->clientip   = NULL;

    free ( bs->spec );
    free ( bs );

    return 1;   /* (success) */
}
