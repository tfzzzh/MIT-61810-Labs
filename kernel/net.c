#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define NUM_PORTS 65536
struct CircularPactQue;
static struct CircularPactQue * port_queue[NUM_PORTS];

// structure for store a package buffer
typedef struct PactBuffer {
  void * addr;
  int length;
  int payload_start; // when payload_start >= length, this payload shall remove from queue
  int payload_end; // end position of the payload (not inclusive)
} PactBuffer;

void initPactBuffer(PactBuffer * this, void * addr, int length) {
  this->addr = addr;
  this->length = length;
  // what if length == payload_start at start? we drop it
  this->payload_start = sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  this->payload_end = sizeof(struct eth) + sizeof(struct ip) + ntohs(EXTRACT_UDP_HEAD(addr)->ulen);
}

// Circular queue for Pacts
#define PACT_QUESIZE 16
typedef struct CircularPactQue {
  struct PactBuffer buffers[PACT_QUESIZE];
  int start;
  int end;
  int len;
} CircularPactQue;

// methods for the queue
// allocator
CircularPactQue* new_queue() {
  printf("init queue takes 1 pages\n");
  return (CircularPactQue*) kalloc();
}

// initalizer
void initCircularPactQue(CircularPactQue* this) {
  this->start = this->end = 0;
  this->len = 0;
  memset(&(this->buffers), 0, PACT_QUESIZE * sizeof(struct PactBuffer));
}

short is_empty(CircularPactQue* this) {
  return this->len == 0;
}

short is_full(CircularPactQue* this) {
  return this->len == PACT_QUESIZE;
}

// assumption not full
void push(CircularPactQue* this, PactBuffer x) {
  this->buffers[this->end] = x;
  this->end = (this->end + 1) % PACT_QUESIZE;
  this->len += 1;
}

// assumption not empty
PactBuffer* front(CircularPactQue* this)
{
  return &(this->buffers[this->start]);
}

// assumption not empty
PactBuffer pop(CircularPactQue* this) {
  // clear head
  PactBuffer x = this->buffers[this->start];
  this->buffers[this->start].addr = NULL;
  this->buffers[this->start].length = 0;
  this->buffers[this->start].payload_start = 0;
  this->buffers[this->start].payload_end = 0;

  this->start = (this->start + 1) % PACT_QUESIZE;
  this->len -= 1;
  return x;
}

// free
void del_queue(CircularPactQue** pthis) {
  kfree(*pthis);
  *pthis = NULL;
}


void
netinit(void)
{
  initlock(&netlock, "netlock");

  // init buffers
  memset(port_queue, 0, sizeof(port_queue));
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  //
  // Your code here.
  //
  // get port
  int port;
  argint(0, &port);
  if (port < 0 || port >= NUM_PORTS) return -1;

  acquire(&netlock);

  // check if this port is used
  if (port_queue[port] != NULL) goto sys_bind_err;

  // create a buffer queue
  port_queue[port] = new_queue();
  if (port_queue[port] == NULL) goto sys_bind_err;
  initCircularPactQue(port_queue[port]);

  release(&netlock);
  return 0;

sys_bind_err:
  release(&netlock);
  return -1;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //
  int port;
  argint(0, &port);
  if (port < 0 || port >= NUM_PORTS) return -1;
  acquire(&netlock);

  if (port_queue[port] == NULL) goto sys_unbind_err;

  // remove
  del_queue(&(port_queue[port]));

  release(&netlock);
  return 0;

sys_unbind_err:
  release(&netlock);
  return -1;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  //
  // Your code here.
  //
  // read parameters
  int dport;
  uint64 src_addr, sport_addr, buf_addr;
  int maxlen; // interface uint32, can wrong for huge uint32
  argint(0, &dport);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &buf_addr);
  argint(4, &maxlen);
  // uint32 *src = (uint32 *) src_addr;
  // uint16 *sport = (uint16 *) sport_addr;
  // char *buf = (char *) buf_addr;

  // check parameters
  if (dport < 0 || dport >= NUM_PORTS) return -1;
  if (src_addr == NULL) return -1;
  if (sport_addr == NULL) return -1;
  if (buf_addr == NULL) return -1;

  // get process's pagetable
  pagetable_t pagetable = myproc()->pagetable;

  uint64 num_recv = 0;
  acquire(&netlock);
  // check if port is binded
  CircularPactQue * que = port_queue[dport];
  if (que == NULL) {
    release(&netlock);
    return -1;
  }

  // wait until queue not empty
  while (is_empty(que)) {
    sleep(que, &netlock);
  }

  // extract one package
  PactBuffer* pact_buff = front(que);
  if (pact_buff->addr == NULL) panic("sys_recv: read null buffer");

  // read package head !!!
  struct ip * ip_head = EXTRACT_IP_HEAD(pact_buff->addr);
  uint32 src = ntohl(ip_head->ip_src);
  struct udp * udp_head = EXTRACT_UDP_HEAD(pact_buff->addr);
  uint16 sport = ntohs(udp_head->sport);
  copyout(pagetable, src_addr, (char*) (&src), sizeof(src)); // handle fail !!!
  copyout(pagetable, sport_addr, (char*) (&sport), sizeof(sport)); 

  // read package payload
  int remain_payload_sz = pact_buff->payload_end - pact_buff->payload_start;
  // printf("parse data with length%d, payload length in udp %d\n", pact_buff->length, ntohs(udp_head->ulen) - (int) sizeof(struct udp));
  // read whole package and remove it from the queue (release memory)
  if (remain_payload_sz <= maxlen) {
    num_recv = remain_payload_sz;
    // memmove(buf, (((char *)pact_buff->addr) + pact_buff->payload_start), num_recv);
    if (remain_payload_sz > 0)
      copyout(pagetable, buf_addr, (((char *)pact_buff->addr) + pact_buff->payload_start), num_recv); // handle fail @@@
    kfree(pact_buff->addr);
    pop(que);
  }
  else {
    num_recv = maxlen;
    // memmove(buf, (((char *)pact_buff->addr) + pact_buff->payload_start), num_recv);
    copyout(pagetable, buf_addr, (((char *)pact_buff->addr) + pact_buff->payload_start), num_recv); // handle fail @@@
    pact_buff->payload_start += maxlen;
  }


  // return number of recieve
  printf("read complete with queue size: %d\n", que->len);
  release(&netlock);
  return num_recv;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

short is_udp_package(char *buff, int len) {
  if (len < sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp)) return 0;
  struct ip *ptr = (struct ip *) (buff + sizeof(struct eth));
  return ptr->ip_p == IPPROTO_UDP;
}

// recv() should wait until a packet for dport arrives
uint16 get_dport_from(char *buff) {
  struct udp *ptr = EXTRACT_UDP_HEAD(buff);
  uint16 port = ntohs(ptr->dport);
  return port;
}

void
ip_rx(char *buf, int len) // buf owned by ip_rx
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  //
  // Your code here.
  //
  // check if the package
  if (!is_udp_package(buf, len)) {
    printf("ip_rx: received an IP which is not udp\n");
    kfree(buf);
    return;
  }

  // check if the buffer is full (should I use lock?) yes
  acquire(&netlock);
  // check if the port has been passed to bind()
  uint16 dport = get_dport_from(buf);
  if (port_queue[dport] == NULL) {
    printf("ip_rx: received an udp to unbind port: %d\n", dport);
    goto ip_rx_err;
  }

  // check if the buffer queue is full
  CircularPactQue * que = port_queue[dport];
  if (is_full(que)) {
    printf("ip_rx: received an udp to port: %d, drop since queue full\n", dport);
    goto ip_rx_err;
  }

  // save buffer into queue
  struct PactBuffer pb;
  initPactBuffer(&pb, buf, len);
  // packet empty
  if (pb.payload_start == pb.length) {
    printf("ip_rx: received an udp to port: %d, drop since package is empty\n", dport);
    goto ip_rx_err;
  }

  push(que, pb); // buf owned by que

  // wakeup ?
  wakeup(que);
  release(&netlock);
  return;

ip_rx_err:
  release(&netlock);
  kfree(buf);
  return;
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
