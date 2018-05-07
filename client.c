#define _GNU_SOURCE
#include <infiniband/verbs.h>
#include <linux/types.h>
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <inttypes.h>

#include "pingpong.h"
#define NUM_SOCKETS 2
#define NUM_PACKETS 50000
#define NUM_PACKETS_WARMUP 1000
#define INITIAL_MESSAGE_SIZE 1
#define FINAL_MESSAGE_SIZE 4096
#define RAISE_SIZE 10
#define TEST_DONE 11
#define LAST_MESSAGE_FOR_TEST 12
#define REGULAR_MESSAGE 13
#define LATENCY_TEST 14

typedef int bool;
#define true 1
#define false 0

static int page_size;
//static int use_odp;
static int use_ts;
//static int validate_buf;
//static int use_dm;

struct pingpong_context {
	struct ibv_context	*context; //the connection context (channels live inside the context)
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr; //this is the memory region we work with
	struct ibv_dm		*dm;
	union {
		struct ibv_cq		*cq;
		struct ibv_cq_ex	*cq_ex;
	} cq_s;
	struct ibv_qp		*qp[NUM_SOCKETS]; //this is the queue pair array we work with
	char			*buf;
	int			 size;
	int			 send_flags;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
	uint64_t		 completion_timestamp_mask;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};
enum ibv_mtu pp_mtu_to_enum(int mtu)
{
	switch (mtu) {
	case 256:  return IBV_MTU_256;
	case 512:  return IBV_MTU_512;
	case 1024: return IBV_MTU_1024;
	case 2048: return IBV_MTU_2048;
	case 4096: return IBV_MTU_4096;
	default:   return 0;
	}
}

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	__be32 v32;
	int i;
	uint32_t tmp_gid[4];

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		tmp_gid[i] = be32toh(v32);
	}
	memcpy(gid, tmp_gid, sizeof(*gid));
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	uint32_t tmp_gid[4];
	int i;

	memcpy(tmp_gid, gid, sizeof(tmp_gid));
	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

int pp_get_port_info(struct ibv_context *context, int port,
		     struct ibv_port_attr *attr)
{
	return ibv_query_port(context, port, attr);
}

static struct ibv_cq *pp_cq(struct pingpong_context *ctx)
{
	return use_ts ? ibv_cq_ex_to_cq(ctx->cq_s.cq_ex) :
		ctx->cq_s.cq;
}
//connect qp_num_to_connect in context to this pingpong dest.
static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx,int qp_num_to_connect)
{
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR,
		.path_mtu		= mtu,
		.dest_qp_num		= dest->qpn,
		.rq_psn			= dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= dest->lid,
			.sl		= sl,
			.src_path_bits	= 0,
			.port_num	= port
		}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(((*ctx).qp[qp_num_to_connect]), &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(((*ctx).qp[qp_num_to_connect]), &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest;
  struct pingpong_dest dest;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;
  printf(servername);
	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
    //printf("wak");
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
            sleep(1);
            
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
            {
                printf("sockfd = %d\n",sockfd);
                printf("broken\n");
				break;
            }
			close(sockfd);
			sockfd = -1;
		}
	}
  //printf("wa");
	freeaddrinfo(res);
  //printf("wawa");
	free(service);
  //printf("wawawa");
  
	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}
    rem_dest = malloc((sizeof dest) * NUM_SOCKETS);
        if (!rem_dest)
            goto out;
    for (int i = 0; i < NUM_SOCKETS; i = i+1)
    {//
        gid_to_wire_gid(&((my_dest[i]).gid), gid);
        //perror("oo");
       sprintf(msg, "%04x:%06x:%06x:%s", my_dest[i].lid, my_dest[i].qpn,
                                my_dest[i].psn, gid);
       printf(msg);
       if (write(sockfd, msg, sizeof msg) != sizeof msg) {
            fprintf(stderr, "Couldn't send local address\n");
            goto out;
       }
       printf("escaped\n");

        if (read(sockfd, msg, sizeof msg) != sizeof msg ||
            write(sockfd, "done", sizeof "done") != sizeof "done") {
            perror("client read/write");
            fprintf(stderr, "Couldn't read/write remote address\n");
            goto out;
        }
//
        

        sscanf(msg, "%x:%x:%x:%s", &rem_dest[i].lid, &rem_dest[i].qpn,
                            &rem_dest[i].psn, gid);
        wire_gid_to_gid(gid, &rem_dest[i].gid);
    }
out:
	close(sockfd);
	return rem_dest;
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port,
					    int use_event)
{
    
	struct pingpong_context *ctx;
	int access_flags = IBV_ACCESS_LOCAL_WRITE;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size       = size;
	ctx->send_flags = IBV_SEND_SIGNALED;
	ctx->rx_depth   = rx_depth;

	ctx->buf = memalign(page_size, size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_ctx;
	}

	memset(ctx->buf, 0x7b, size);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_buffer;
	}

	if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			goto clean_device;
		}
	} else
		ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}


	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, access_flags);

	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_dm;
	}

		ctx->cq_s.cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
					     ctx->channel, 0);
	

	if (!pp_cq(ctx)) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;
	}
    for (int i = 0 ; i < NUM_SOCKETS; i = i+1) //create NUM_SOCKETS QP's
	{
		struct ibv_qp_attr attr;
		struct ibv_qp_init_attr init_attr = {
			.send_cq = pp_cq(ctx),
			.recv_cq = pp_cq(ctx),
			.cap     = {
				.max_send_wr  = 1,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC
		};

		((*ctx).qp[i]) = ibv_create_qp(ctx->pd, &init_attr);////////////
		if (!((*ctx).qp[i]))  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}

		ibv_query_qp(((*ctx).qp[i]), &attr, IBV_QP_CAP, &init_attr);
		if (init_attr.cap.max_inline_data >= size) {
			ctx->send_flags |= IBV_SEND_INLINE;
		}
	

	
		struct ibv_qp_attr attr2 = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qp_access_flags = 0
		};

		if (ibv_modify_qp((*ctx).qp[i], &attr2,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
	}

	return ctx;

clean_qp:
    for (int k = 0 ; k < NUM_SOCKETS; k = k+1)
        ibv_destroy_qp((*ctx).qp[k]);

clean_cq:
	ibv_destroy_cq(pp_cq(ctx));

clean_mr:
	ibv_dereg_mr(ctx->mr);

clean_dm:
	if (ctx->dm)
		//ibv_free_dm(ctx->dm);

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_device:
	ibv_close_device(ctx->context);

clean_buffer:
	free(ctx->buf);

clean_ctx:
	free(ctx);

	return NULL;
}


static int pp_close_ctx(struct pingpong_context *ctx)
{
    for (int k = 0 ; k < NUM_SOCKETS; k = k+1)
        if ( ibv_destroy_qp((*ctx).qp[k])) {
            fprintf(stderr, "Couldn't destroy QP\n");
            return 1;
        }

	if (ibv_destroy_cq(pp_cq(ctx))) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ctx->dm) {
		//if (ibv_free_dm(ctx->dm)) {
		//	fprintf(stderr, "Couldn't free DM\n");
		//	return 1;
		//}
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

static int pp_post_recv(struct pingpong_context *ctx, int n,int qp_num)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv((*ctx).qp[qp_num], &wr, &bad_wr))
			break;

	return i;
}

static int pp_post_send(struct pingpong_context *ctx,int qp_num,int imm_data)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND_WITH_IMM,
		.send_flags = ctx->send_flags,
        .imm_data = htonl(imm_data),
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send((*ctx).qp[qp_num], &wr, &bad_wr);
}

int main(int argc, char *argv[])
{
    
    struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_context *context;
	struct pingpong_dest    my_dest[NUM_SOCKETS];
	struct pingpong_dest    *rem_dest;
	struct timeval           timer;
	char                    *ib_devname = NULL;
	char                    *servername = NULL;
	unsigned int             port = 18515;
	int                      ib_port = 1;
	int             messageSize[NUM_SOCKETS];
    int             numMessages[NUM_SOCKETS];
    page_size = sysconf(_SC_PAGESIZE); //checks the page size used by the system
    for (int i=0 ;i< NUM_SOCKETS; i = i+1)
    {
        messageSize[i] = page_size; //start with page_size messages
        numMessages[i] = 10000; //start with 10000 iters per size of message
    }
	enum ibv_mtu		 mtu = IBV_MTU_1024;
	unsigned int             rx_depth = 500;
	
	int                      use_event = 0;
	int                      routs[NUM_SOCKETS];
	//int                      rcnt[NUM_SOCKETS];
    int                      sendCount[NUM_SOCKETS];
	int                      num_cq_events;//only one completion queue
	int                      sl = 0;
	int			 gidx = -1;
	char			 gid[33];
	//struct ts_params	 ts;

	srand48(getpid() * time(NULL));

    
    //get input for the server ip and port
    int portNum;
    int numArgs = 3;
    char* usageMessage = "usage %s Server IP port\n";
	if (argc < numArgs) {
       fprintf(stderr,usageMessage, argv[0]);
       exit(0);
    }
    portNum = atoi(argv[numArgs - 1]);
    port = portNum;
    servername = strdupa(argv[1]);
    
    //get our beloved device
    dev_list = ibv_get_device_list(NULL); //get devices available to this machine
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return 1;
	}
    ib_dev = *dev_list; //chooses the first device by default
    if (!ib_dev) {
        fprintf(stderr, "No IB devices found\n");
        return 1;
    }
    //Create the context for this connection
    //creates context on found device, registers memory of size.
    context = pp_init_ctx(ib_dev, FINAL_MESSAGE_SIZE, rx_depth, ib_port, use_event); //use_event (decides if we wait blocking for completion)
    if (!context)
        return 1;
    
    if (pp_get_port_info(context->context, ib_port, &context->portinfo)) { //gets the port status and info (uses ibv_query_port)
            fprintf(stderr, "Couldn't get port info\n");
            return 1;
        }
    for ( int k = 0; k < NUM_SOCKETS ; k = k+1)
    {
        //Prepare to recieve messages. fill the recieve request queue of QP k
        routs[k] = pp_post_recv(context, context->rx_depth,k); //post rx_depth recieve requests
            if (routs[k] < context->rx_depth) {
                fprintf(stderr, "Couldn't post receive (%d)\n", routs[k]);
                return 1;
            }
        //set my_dest for every QP, getting ready to connect them.
        my_dest[k].lid = context->portinfo.lid; //assigns lid to my dest
        if (context->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
                                !my_dest[k].lid) {
            fprintf(stderr, "Couldn't get local LID\n");
            return 1;
        }
        //set the gid to 0, we are in the same subnet.
        memset(&my_dest[k].gid, 0, sizeof my_dest[k].gid); //zero the gid, we send in the same subnet
        my_dest[k].qpn = ((*context).qp[k])->qp_num; //gets the qp number
        my_dest[k].psn = lrand48() & 0xffffff; //randomizes the packet serial number
        inet_ntop(AF_INET6, &my_dest[k].gid, gid, sizeof gid); //changes gid to text form
        printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
               my_dest[k].lid, my_dest[k].qpn, my_dest[k].psn, gid);
    }
    //Get the remote dest for my QPs
    rem_dest = pp_client_exch_dest(servername, port, my_dest); //if youre a client - exchange data with server
    if (!rem_dest)
            return 1; 
    

    for(int k = 0 ; k < NUM_SOCKETS; k = k + 1)
    {
      inet_ntop(AF_INET6, &rem_dest[k].gid, gid, sizeof gid);
      printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
             rem_dest[k].lid, rem_dest[k].qpn, rem_dest[k].psn, gid);
    }      
    //now connect all the QPs to the server
    for( int k = 0 ; k < NUM_SOCKETS; k = k+1)
    {
        if (pp_connect_ctx(context, ib_port, my_dest[k].psn, mtu, sl, &rem_dest[k],
                        gidx,k))
                return 1; //connect to the server

    }
    //context->pending = PINGPONG_RECV_WRID; //TODO understand what this does, probably sets context to recieve data
    printf("all sockets connected OMG \n");
    
    ///////////
    /*for(int i = 0 ; i < NUM_SOCKETS ; i = i + 1)
    {
        if (pp_post_send(context,i,15)) { //TODO understand this
                    fprintf(stderr, "Couldn't post send\n");
                    return 1;
                }
    }*/
    int ret;
    int ne, i;
    struct ibv_wc wc[NUM_SOCKETS];
    /////////////////////////////////////////////////////
    
    ////framework for test
    bool testDone[NUM_SOCKETS];
    bool latencyDone[NUM_SOCKETS];
    bool raiseNum[NUM_SOCKETS];
    bool allDone = false;
    bool gotAnswer[NUM_SOCKETS];
    int packetCounter[NUM_SOCKETS];
    int numPackets[NUM_SOCKETS];
    long lastTime[NUM_SOCKETS];
    long thisTime[NUM_SOCKETS];
    long startTime[NUM_SOCKETS];
    long endTime[NUM_SOCKETS];
    long latency[NUM_SOCKETS];
    //int messageSize[NUM_SOCKETS];
    int leftToSend[NUM_SOCKETS];
    for(int i = 0;i<NUM_SOCKETS;i = i+1) //Initialize all the sockets for the program
	{
     
        testDone[i] = false;
        //localDone[i] = false;
        packetCounter[i] = 0;
        numPackets[i] = numMessages[i];
        lastTime[i] = 1;
        thisTime[i] = 100;
        raiseNum[i] = false;
        latency[i] = 0;
        messageSize[i] = INITIAL_MESSAGE_SIZE;    
        leftToSend[i] = INITIAL_MESSAGE_SIZE;
        gotAnswer[i] = true;
	}
    //////
    
    while(!allDone)
    {
    //////
        allDone = true;
        //send another round of packetsss
        for ( int k = 0; k < NUM_SOCKETS; k = k+1)
        {
            if(testDone[k])
            {
                //printf("continued\n");
                continue;
            }
            else 
            {
                allDone = false;
            }
            if(!gotAnswer[k])
                continue;
            if(packetCounter[k] == 0)
            {
                if (gettimeofday(&timer, NULL)) {
                    perror("gettimeofday");
                    return 1;
                }
                printf("timer started on %d\n",k);
                startTime[k] = (long) timer.tv_sec * 1000000 + (long)timer.tv_usec;
            }
            int message_type = REGULAR_MESSAGE;
            if(packetCounter[k] == numPackets[k] - 1)
            {
                printf("last msg ofr this test %d\n",k);
                message_type = LAST_MESSAGE_FOR_TEST;
            }
            else if (packetCounter[k] == numPackets[k])
            {
                message_type = RAISE_SIZE;
                messageSize[k] = 2 * messageSize[k];
                printf("raisin size %d\n",k);
                if(messageSize[k] > FINAL_MESSAGE_SIZE)
                {
                    testDone[k] = true;
                    message_type = TEST_DONE;
                }
            }
            else if (!latencyDone[k] && packetCounter[k] == NUM_PACKETS_WARMUP)
            {
                if (gettimeofday(&timer, NULL)) {
                perror("gettimeofday");
                return 1;
                }
                latency[k] = (long) timer.tv_sec * 1000000 + (long)timer.tv_usec;
                message_type = LATENCY_TEST;
                printf("testin latency on %d\n",k);
                //latencyDone[k] = true;
            }
            
            if (pp_post_send(context,k,message_type)) { //TODO understand this
                    fprintf(stderr, "Couldn't post sendoo\n");
                    return 1;
                }
            gotAnswer[k] = false;
        }
        //do what needs to be done with work completes 
        ne = ibv_poll_cq(pp_cq(context), NUM_SOCKETS, wc);
        if (ne < 0) {
            fprintf(stderr, "poll CQ failed %d\n", ne);
            return 1;
        }
        for (i = 0; i < ne; ++i) {//does what needs to be done
            if (wc[i].status != IBV_WC_SUCCESS) 
            {
                fprintf(stderr, "Failed status %d\n", wc[i].status);
                return 1;
            }
            //find the QP index the message was recieved in
            int qpNum;
            for(qpNum = 0; qpNum < NUM_SOCKETS; qpNum = qpNum + 1)
            {
                //printf("qp checked = %d, vs recieved %d\n",my_dest[qpNum],wc[i].qp_num);
                if(my_dest[qpNum].qpn == wc[i].qp_num)
                    break;
            }
            if ((int)wc[i].wr_id == PINGPONG_SEND_WRID)
            {
                packetCounter[qpNum] = packetCounter[qpNum] + 1;
                gotAnswer[qpNum] = true;
            }
            else if((int)wc[i].wr_id == PINGPONG_RECV_WRID)//this is a recv operation completed.
            {
                //post recv
                routs[qpNum] = routs[qpNum] -1 + pp_post_recv(context, 1,qpNum); //post rx_depth recieve requests
                if (routs[qpNum] < context->rx_depth) {
                    fprintf(stderr, "Couldn't post receive (%d)\n", routs[qpNum]);
                    return 1;
                }
                     //do specific work according to protocol
                int imm_data = htonl(wc[i].imm_data);
                printf("imm_data = %d\n",imm_data);
                if(imm_data == LAST_MESSAGE_FOR_TEST)
                {
                    //TODO stop timer and print res 
                    packetCounter[qpNum] = 0;
                    if (gettimeofday(&timer, NULL)) {
                        perror("gettimeofday");
                        return 1;
                    }
                    endTime[qpNum] =(long) timer.tv_sec * 1000000 + (long)timer.tv_usec;
                    printf("qp: %d test was done, took %ld ms\n",qpNum,((double)(endTime[qpNum] - startTime[qpNum]))/1000.0);
                }
                else if(imm_data == TEST_DONE)
                {
                    testDone[qpNum] = true;
                }
                else if(imm_data == LATENCY_TEST)
                {
                    if (gettimeofday(&timer, NULL)) {
                        perror("gettimeofday");
                        return 1;
                    }
                    latency[qpNum] =latency[qpNum] - (long) timer.tv_sec * 1000000 + (long)timer.tv_usec;
                    latencyDone[qpNum] = true;
                    printf("qp: %d latency was done, took %ld ms\n",qpNum,((double)(latency[qpNum]))/1000.0);

                }
            }
            else
            {
                perror("failed send or recv ><");
                return 1;
            }
        
        }
            //send recieve logic
            //rcnt[k] = scnt[k] = 0;
            /*
            if (rcnt[k] < iters[k] || scnt[k] < iters[k]) {
                    if(rcnt[k] == 0)
                    {
                        startTime[k] = (long) start[k].tv_sec * 1000000 + (long)start[k].tv_usec;
                    }
                    int ret;
                    int ne, i;
                    struct ibv_wc wc[2];

                    //do {//loop until we have a work completions
                        ne = ibv_poll_cq(pp_cq(contexts[k]), 2, wc);
                        if (ne < 0) {
                            fprintf(stderr, "poll CQ failed %d\n", ne);
                            return 1;
                    //    }
                    //} while (!use_event && ne < 1);

                    for (i = 0; i < ne; ++i) {//does what needs to be done with these WCs
                        ret = parse_single_wc(contexts[k], &scnt[k], &rcnt[k], &routs[k],
                                      iters[k],
                                      wc[i].wr_id,
                                      wc[i].status,
                                      0, &ts);
                        if (ret) {
                            fprintf(stderr, "parse WC failed %d\n", ne);
                            return 1;
                        }
                    }
                //}
            }
            else //done packets for this test, measure time
            {
                if (gettimeofday(&end[k], NULL)) {
                perror("gettimeofday");
                return 1;
                }
                endTime[k] = (long) end[k].tv_sec * 1000000 + (long)end[k].tv_usec;
                //localDone[k] = true;
                lastTime[k] = thisTime[k];
                thisTime[k] = endTime[k] - startTime[k];
                //TODO print result
                printf("test %d done, time: %ld\n",k,thisTime[k]);
                //raise message size
                size[k] = 2*size[k];
                lastTime[k] = 1;
                thisTime[k] = 100;
                rcnt[k] = 0;
                scnt[k] = 0;
                if(size[k] > FINAL_MESSAGE_SIZE)
                {
                    testDone[k] = true;
                    if (pp_close_ctx(contexts[k]))
                        return 1;
                    continue;
                }
            }
            
            //time calculation

            if(false)//TODO decide what to do with this code
            {
                float usec = (end[k].tv_sec - start[k].tv_sec) * 1000000 +
                    (end[k].tv_usec - start[k].tv_usec);
                long long bytes = (long long) size[k] * iters[k] * 2;

                printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
                       bytes, usec / 1000000., bytes * 8. / usec);
                printf("%d iters in %.2f seconds = %.2f usec/iter\n",
                       iters[k], usec / 1000000., usec / iters[k]);

                if ((!servername) && (validate_buf)) {
                    
                    for (int i = 0; i < size[k]; i += page_size)
                        if (contexts[k]->buf[i] != i / page_size % sizeof(char))
                            printf("invalid data in page %d\n",
                                   i / page_size);
                }
            }

            //ibv_ack_cq_events(pp_cq(contexts[k]), num_cq_events[k]);

            
        }*/
    }
    
    ////////////////////////////////////////////////////
    //do {//loop until we have a work completions
    sleep(1);
    //ne = ibv_poll_cq(pp_cq(context), 2, wc);
    //printf("we have %d completion messages waiting!!!",ne);
    //printf("imm data is: %d and %d\n", wc[0].qp_num,wc[1].qp_num);
    /////////////
    
    printf("freeing data");
   	ibv_free_device_list(dev_list);
    free(rem_dest);

}
    