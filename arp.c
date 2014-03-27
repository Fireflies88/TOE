/*  Copyright (C) 2011-2013  P.D. Buchan (pdbuchan@yahoo.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Send an IPv4 ARP packet via raw socket at the link layer (ethernet frame).
// Values set for ARP request.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>           // close()
#include <string.h>           // strcpy, memset(), and memcpy()
#include <netdb.h>            // struct addrinfo
#include <sys/types.h>        // needed for socket(), uint8_t, uint16_t
#include <sys/socket.h>       // needed for socket()
#include <netinet/in.h>       // IPPROTO_RAW, INET_ADDRSTRLEN
#include <netinet/ip.h>       // IP_MAXPACKET (which is 65535)
#include <netinet/tcp.h>  
#define __FAVOR_BSD
#include <arpa/inet.h>        // inet_pton() and inet_ntop()
#include <sys/ioctl.h>        // macro ioctl is defined
#include <bits/ioctls.h>      // defines values for argument "request" of ioctl.
#include <net/if.h>           // struct ifreq
#include <linux/if_ether.h>   // ETH_P_ARP = 0x0806
#include <linux/if_packet.h>  // struct sockaddr_ll (see man 7 packet)
#include <net/ethernet.h>
#include <errno.h>            // errno, perror()

// ARP header
typedef struct _arp_hdr arp_hdr;
struct _arp_hdr {
  uint16_t htype;
  uint16_t ptype;
  uint8_t hlen;
  uint8_t plen;
  uint16_t opcode;
  uint8_t sender_mac[6];
  uint8_t sender_ip[4];
  uint8_t target_mac[6];
  uint8_t target_ip[4];
};

#define ETH_HDRLEN 14      // Ethernet header length
#define TCP_HDRLEN 20
#define IP4_HDRLEN 20      // IPv4 header length
#define ARP_HDRLEN 28      // ARP header length
#define ARPOP_REQUEST 1    // Taken from <linux/if_arp.h>
#define ARPOP_REPLY 2	

char *allocate_strmem (int);
uint8_t *allocate_ustrmem (int);
int *allocate_intmem (int);

int interface_lookup(char*, char*, struct ifreq*, uint8_t *, struct sockaddr_ll*); 
int listen_ARP(int, uint8_t *, arp_hdr *, uint8_t *); 
int fill_ARPhdr(arp_hdr *, uint8_t *);

uint16_t checksum (uint16_t *, int);

int main (int argc, char **argv)
{

  printf("Starting\n");

  int sd;
  char *interface, *target, *src_ip, *dst_ip;
  arp_hdr arphdr_out;
  uint8_t *src_mac, *dst_mac, *ether_frame;
  struct addrinfo hints, *res;
  struct sockaddr_ll device;
  struct ifreq ifr;

  struct ip iphdr; 
  int *ip_flags; 
  int status;

  // Allocate memory for various arrays.
  src_mac = allocate_ustrmem(6);
  dst_mac = allocate_ustrmem(6);
  ether_frame = allocate_ustrmem(IP_MAXPACKET);
  interface = allocate_strmem(40);
  target = allocate_strmem(40);
  src_ip = allocate_strmem(INET_ADDRSTRLEN);
  dst_ip = allocate_strmem(INET_ADDRSTRLEN);
  ip_flags = allocate_intmem (4);
    	
  // Look-up interface
  interface_lookup(interface, "wlan0", &ifr, src_mac, &device);

  // Set destination MAC address: broadcast address
  memset (dst_mac, 0xff, 6 * sizeof (uint8_t));

  // Resolve ipv4 url if needed
  config_ipv4(src_ip, "160.39.10.141", target, "www.google.com", src_mac, &hints, res, &arphdr_out, &device, dst_ip);

  // Fill out ARP packet
  fill_ARPhdr(&arphdr_out, src_mac);
    
  sd = fill_send_ETHhdr(ether_frame, dst_mac, src_mac, &arphdr_out, &device);

  listen_ARP(sd, ether_frame, &arphdr_out, dst_mac);

  //IPV4 header
  // IPv4 header length (4 bits): Number of 32-bit words in header = 5
  iphdr.ip_hl = IP4_HDRLEN / sizeof (uint32_t);

  // Internet Protocol version (4 bits): IPv4
  iphdr.ip_v = 4;

  // Type of service (8 bits)
  iphdr.ip_tos = 0;

  // Total length of datagram (16 bits): IP header + TCP header
  iphdr.ip_len = htons (IP4_HDRLEN + TCP_HDRLEN);

  // ID sequence number (16 bits): unused, since single datagram
  iphdr.ip_id = htons (0);

  // Flags, and Fragmentation offset (3, 13 bits): 0 since single datagram

  // Zero (1 bit)
  ip_flags[0] = 0;

  // Do not fragment flag (1 bit)
  ip_flags[1] = 0;

  // More fragments following flag (1 bit)
  ip_flags[2] = 0;

  // Fragmentation offset (13 bits)
  ip_flags[3] = 0;

  iphdr.ip_off = htons ((ip_flags[0] << 15)
                      + (ip_flags[1] << 14)
                      + (ip_flags[2] << 13)
                      +  ip_flags[3]);

  // Time-to-Live (8 bits): default to maximum value
  iphdr.ip_ttl = 255;

  // Transport layer protocol (8 bits): 6 for TCP
  iphdr.ip_p = IPPROTO_TCP;

  // Source IPv4 address (32 bits)
  if ((status = inet_pton (AF_INET, src_ip, &(iphdr.ip_src))) != 1) {
    fprintf (stderr, "inet_pton() failed 1.\nError message: %s", strerror (status));
    exit (EXIT_FAILURE);
  }

  // Destination IPv4 address (32 bits)
  if ((status = inet_pton (AF_INET, dst_ip, &(iphdr.ip_dst))) != 1) {
    fprintf (stderr, "inet_pton() failed 2.\nError message: %s", strerror (status));
    exit (EXIT_FAILURE);
  }

  // IPv4 header checksum (16 bits): set to 0 when calculating checksum
  iphdr.ip_sum = 0;
  iphdr.ip_sum = checksum ((uint16_t *) &iphdr, IP4_HDRLEN);
  // Close socket descriptor.
  close (sd);

  // Free allocated memory.
  free (src_mac);
  free (dst_mac);
  free (ether_frame);
  free (interface);
  free (target);
  free (src_ip);

  return (EXIT_SUCCESS);
}

int fill_ARPhdr(arp_hdr *arphdr_out, uint8_t *src_mac) 
{
  // Hardware type (16 bits): 1 for ethernet
  arphdr_out->htype = htons (1);

  // Protocol type (16 bits): 2048 for IP
  arphdr_out->ptype = htons (ETH_P_IP);

  // Hardware address length (8 bits): 6 bytes for MAC address
  arphdr_out->hlen = 6;

  // Protocol address length (8 bits): 4 bytes for IPv4 address
  arphdr_out->plen = 4;

  // OpCode: 1 for ARP request
  arphdr_out->opcode = htons (ARPOP_REQUEST);

  // Sender hardware address (48 bits): MAC address
  memcpy (&arphdr_out->sender_mac, src_mac, 6 * sizeof (uint8_t));

  // Sender protocol address (32 bits)
  // See getaddrinfo() resolution of src_ip.

  // Target hardware address (48 bits): zero, since we don't know it yet.
  memset (&arphdr_out->target_mac, 0, 6 * sizeof (uint8_t));

  // Target protocol address (32 bits)
  // See getaddrinfo() resolution of target.
  
  return 0;
}

int fill_send_ETHhdr(uint8_t *ether_frame, uint8_t *dst_mac, uint8_t *src_mac, arp_hdr *arphdr_out, struct sockaddr_ll *device) 
{
  int sd, frame_length, bytes;
  // Fill out ethernet frame header.

  // Ethernet frame length = ethernet header (MAC + MAC + ethernet type) + ethernet data (ARP header)
  frame_length = 6 + 6 + 2 + ARP_HDRLEN;

  // Destination and Source MAC addresses
  memcpy (ether_frame, dst_mac, 6 * sizeof (uint8_t));
  memcpy (ether_frame + 6, src_mac, 6 * sizeof (uint8_t));

  // Next is ethernet type code (ETH_P_ARP for ARP).
  // http://www.iana.org/assignments/ethernet-numbers
  ether_frame[12] = ETH_P_ARP / 256;
  ether_frame[13] = ETH_P_ARP % 256;

  // Next is ethernet frame data (ARP header).

  // ARP header
  memcpy (ether_frame + ETH_HDRLEN, arphdr_out, ARP_HDRLEN * sizeof (uint8_t));

  // Submit request for a raw socket descriptor.
  if ((sd = socket (PF_PACKET, SOCK_RAW, htons (ETH_P_ALL))) < 0) {
    perror ("socket() failed ");
    exit (EXIT_FAILURE);
  }

  // Send ethernet frame to socket.
  if ((bytes = sendto (sd, ether_frame, frame_length, 0, (struct sockaddr *) device, sizeof (struct sockaddr_ll))) <= 0) {
    perror ("sendto() failed");
    exit (EXIT_FAILURE);
  }	

  return sd;
}


int config_ipv4(char* src_ip, char* src_ip_addr, char* target, char* trg_ip_addr, uint8_t *src_mac, struct addrinfo *hints, struct addrinfo *res, arp_hdr *arphdr_out, struct sockaddr_ll *device, char* dst_ip) 
{
  int status;
  struct sockaddr_in *ipv4;
  void *tmp;

  // Source IPv4 address:  you need to fill this out
  strcpy (src_ip, src_ip_addr);

  // Destination URL or IPv4 address (must be a link-local node): you need to fill this out
  strcpy (target, trg_ip_addr);

  // Fill out hints for getaddrinfo().
  memset (hints, 0, sizeof (struct addrinfo));
  hints->ai_family = AF_INET;
  hints->ai_socktype = SOCK_STREAM;
  hints->ai_flags = hints->ai_flags | AI_CANONNAME;

  // Source IP address
  if ((status = inet_pton (AF_INET, src_ip, arphdr_out->sender_ip)) != 1) {
    fprintf (stderr, "inet_pton() source IP address.\nError message: %s", strerror (status));
    exit (EXIT_FAILURE);
  }

  // Resolve target using getaddrinfo().
  if ((status = getaddrinfo (target, NULL, hints, &res)) != 0) {
    fprintf (stderr, "getaddrinfo() failed: %s\n", gai_strerror (status));
    exit (EXIT_FAILURE);
  }
  ipv4 = (struct sockaddr_in *) res->ai_addr;
  tmp = &(ipv4->sin_addr);
  memcpy (arphdr_out->target_ip, tmp, 4 * sizeof (uint8_t));
  if (inet_ntop (AF_INET, tmp, dst_ip, INET_ADDRSTRLEN) == NULL) {
	status = errno;
	fprintf (stderr, "inet_ntop() failed.\n Error message: %s", strerror(status));
	exit(EXIT_FAILURE);
  }  

  freeaddrinfo (res);

  // Fill out sockaddr_ll.
  device->sll_family = AF_PACKET;
  memcpy (device->sll_addr, src_mac, 6 * sizeof (uint8_t));
  device->sll_halen = htons (6);
	
  return 0;
}

int listen_ARP(int sd, uint8_t *ether_frame, arp_hdr *arphrd_out, uint8_t *dst_mac) 
{
  // Listen for incoming ethernet frame from socket sd.
  // We expect an ARP ethernet frame of the form:
  //     MAC (6 bytes) + MAC (6 bytes) + ethernet type (2 bytes)
  //     + ethernet data (ARP header) (28 bytes)
  // Keep at it until we get an ARP reply.

  printf("Receiving ... \n");

  int status, i;
  arp_hdr *arp_pt_in;
  arp_pt_in = (arp_hdr *) (ether_frame + 6 + 6 + 2);
  while (((((ether_frame[12]) << 8) + ether_frame[13]) != ETH_P_ARP) || (ntohs (arp_pt_in->opcode) != ARPOP_REPLY)) {
    if ((status = recv (sd, ether_frame, IP_MAXPACKET, 0)) < 0) {
      if (errno == EINTR) {
        memset (ether_frame, 0, IP_MAXPACKET * sizeof (uint8_t));
        continue;  // Something weird happened, but let's try again.
      } else {
        perror ("recv() failed:");
        exit (EXIT_FAILURE);
      }
    }
  }

  // DEBBUG - TO BE COMMENTED
  // Print out contents of received ethernet frame.
  printf ("\nEthernet frame header:\n");
  printf ("Destination MAC (this node): ");
  for (i=0; i<5; i++) {
    printf ("%02x:", ether_frame[i]);
  }
  printf ("%02x\n", ether_frame[5]);
  printf ("Source MAC: ");
  for (i=0; i<5; i++) {
    printf ("%02x:", ether_frame[i+6]);
  }
  printf ("%02x\n", ether_frame[11]);
  // Next is ethernet type code (ETH_P_ARP for ARP).
  // http://www.iana.org/assignments/ethernet-numbers
  printf ("Ethernet type code (2054 = ARP): %u\n", ((ether_frame[12]) << 8) + ether_frame[13]);
  printf ("\nEthernet data (ARP header):\n");
  printf ("Hardware type (1 = ethernet (10 Mb)): %u\n", ntohs (arp_pt_in->htype));
  printf ("Protocol type (2048 for IPv4 addresses): %u\n", ntohs (arp_pt_in->ptype));
  printf ("Hardware (MAC) address length (bytes): %u\n", arp_pt_in->hlen);
  printf ("Protocol (IPv4) address length (bytes): %u\n", arp_pt_in->plen);
  printf ("Opcode (2 = ARP reply): %u\n", ntohs (arp_pt_in->opcode));
  printf ("Sender hardware (MAC) address: ");
  for (i=0; i<5; i++) {
    printf ("%02x:", arp_pt_in->sender_mac[i]);
  }
  printf ("%02x\n", arp_pt_in->sender_mac[5]);
  printf ("Sender protocol (IPv4) address: %u.%u.%u.%u\n",
    arp_pt_in->sender_ip[0], arp_pt_in->sender_ip[1], arp_pt_in->sender_ip[2], arp_pt_in->sender_ip[3]);
  printf ("Target (this node) hardware (MAC) address: ");
  for (i=0; i<5; i++) {
    printf ("%02x:", arp_pt_in->target_mac[i]);
  }
  printf ("%02x\n", arp_pt_in->target_mac[5]);
  printf ("Target (this node) protocol (IPv4) address: %u.%u.%u.%u\n",
    arp_pt_in->target_ip[0], arp_pt_in->target_ip[1], arp_pt_in->target_ip[2], arp_pt_in->target_ip[3]);

  for (i = 0; i < 6; i++) dst_mac[i] = arp_pt_in-> sender_mac[i];
  printf("dst_mac : ");
  for (i = 0; i < 6; i++) printf("%02x:", dst_mac[i]);  
  printf("\n"); 
  return 0;
}


// Allocate memory for an array of chars.
char *allocate_strmem (int len)
{
  void *tmp;

  if (len <= 0) {
    fprintf (stderr, "ERROR: Cannot allocate memory because len = %i in allocate_strmem().\n", len);
    exit (EXIT_FAILURE);
  }

  tmp = (char *) malloc (len * sizeof (char));
  if (tmp != NULL) {
    memset (tmp, 0, len * sizeof (char));
    return (tmp);
  } else {
    fprintf (stderr, "ERROR: Cannot allocate memory for array allocate_strmem().\n");
    exit (EXIT_FAILURE);
  }
}

// Allocate memory for an array of unsigned chars.
uint8_t *allocate_ustrmem (int len)
{
  void *tmp;

  if (len <= 0) {
    fprintf (stderr, "ERROR: Cannot allocate memory because len = %i in allocate_ustrmem().\n", len);
    exit (EXIT_FAILURE);
  }

  tmp = (uint8_t *) malloc (len * sizeof (uint8_t));
  if (tmp != NULL) {
    memset (tmp, 0, len * sizeof (uint8_t));
    return (tmp);
  } else {
    fprintf (stderr, "ERROR: Cannot allocate memory for array allocate_ustrmem().\n");
    exit (EXIT_FAILURE);
  }
}

int interface_lookup(char *interface, char *name, struct ifreq *ifr, uint8_t *src_mac, struct sockaddr_ll *device)
{
   int sd;
   printf("Looking up interface\n");
   strcpy(interface, name);
  
   // Submit request for a socket descriptor to look up interface.
   if ((sd = socket (AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
     perror ("socket() failed to get socket descriptor for using ioctl() ");
     exit (EXIT_FAILURE);
   }

   // Use ioctl() to look up interface name and get its MAC address.
   memset (ifr, 0, sizeof (*ifr));
   snprintf (ifr->ifr_name, sizeof (ifr->ifr_name), "%s", interface);
   if (ioctl (sd, SIOCGIFHWADDR, ifr) < 0) {
     perror ("ioctl() failed to get source MAC address ");
     return (EXIT_FAILURE);
   }
   close (sd);
  
   // Copy source MAC address.
   memcpy (src_mac, ifr->ifr_hwaddr.sa_data, 6 * sizeof (uint8_t));

   // Report source MAC address to stdout.
   int i;
   printf ("MAC address for interface %s is ", interface);
   for (i=0; i<5; i++) {
     printf ("%02x:", src_mac[i]);
   }
   printf ("%02x\n", src_mac[5]);

   // Find interface index from interface name and store index in
   // struct sockaddr_ll device, which will be used as an argument of sendto().
   memset (device, 0, sizeof (device));
   if ((device->sll_ifindex = if_nametoindex (interface)) == 0) {
     perror ("if_nametoindex() failed to obtain interface index ");
     exit (EXIT_FAILURE);
   }
   printf ("Index for interface %s is %i\n", interface, device->sll_ifindex);

   return 0;
} 


// Allocate memory for an array of ints.
int *allocate_intmem (int len)
{
  void *tmp;

  if (len <= 0) {
    fprintf (stderr, "ERROR: Cannot allocate memory because len = %i in allocate_intmem().\n", len);
    exit (EXIT_FAILURE);
  }

  tmp = (int *) malloc (len * sizeof (int));
  if (tmp != NULL) {
    memset (tmp, 0, len * sizeof (int));
    return (tmp);
  } else {
    fprintf (stderr, "ERROR: Cannot allocate memory for array allocate_intmem().\n");
    exit (EXIT_FAILURE);
  }
}

// Checksum function
uint16_t checksum (uint16_t *addr, int len)
{
  int nleft = len;
  int sum = 0;
  uint16_t *w = addr;
  uint16_t answer = 0;

  while (nleft > 1) {
    sum += *w++;
    nleft -= sizeof (uint16_t);
  }

  if (nleft == 1) {
    *(uint8_t *) (&answer) = *(uint8_t *) w;
    sum += answer;
  }

  sum = (sum >> 16) + (sum & 0xFFFF);
  sum += (sum >> 16);
  answer = ~sum;
  return (answer);
}
