/*
 * nat624{cc,hh} -- element that translate an IPv6 packet to an IPv4 packet
 * 
 * Peilei Fan
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology.
 *
 * This software is being provided by the copyright holders under the GNU
 * General Public License, either version 2 or, at your discretion, any later
 * version. For more information, see the `COPYRIGHT' file in the source
 * distribution.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include "nat624.hh"
#include "confparse.hh"
#include "error.hh"
#include "click_ip.h"
#include "click_ip6.h"
#include "click_icmp.h"
#include "click_icmp6.h"
#include "ip6address.hh"

Nat624::Nat624()
{
  add_input();
  add_output();
}

Nat624::~Nat624()
{
}

void
Nat624::add_map(IPAddress ipa4, IP6Address ipa6, bool bound)
{ 
  struct Entry64 e;
  e._ip6=ipa6;
  e._ip4=ipa4;
  e._bound = bound;
  _v.push_back(e);
}


int
Nat624::configure(const Vector<String> &conf, ErrorHandler *errh)
{
  _v.clear();

  int before= errh->nerrors();
  for (int i = 0; i<conf.size(); i++) 
    {
      IP6Address ipa6;
      IPAddress ipa4;

      Vector<String> words;
      cp_spacevec(conf[i], words);

      for (int j = 0; j < words.size(); j++)
	if (cp_ip_address(words[j], (unsigned char *)&ipa4)) {
	  if (j < words.size() - 1
	      && cp_ip6_address(words[j+1], (unsigned char *)&ipa6)) {
	    add_map(ipa4, ipa6, 1);
	    j++;
	  } else
	    add_map(ipa4, IP6Address("::0"), 0);
	}
    }
  return (before ==errh->nerrors() ? 0: -1);
}


//check if there's already binding for the ipv6 address, if not, then assign the first unbinded ipv4 address to it.  If there's no ipv4 address available, return false.  

bool
Nat624::lookup(IP6Address ipa6, IPAddress &ipa4)
{ 
  int i = -1;

  for (i=0; i<_v.size(); i++) {
    if ((ipa6 == _v[i]._ip6) && (_v[i]._bound)) {
      // click_chatter("nat624::lookup -1");
      ipa4 = _v[i]._ip4;
      return (true);
    }
    else if ((ipa6 == _v[i]._ip6 ) && (_v[i]._bound==0)) {
      _v[i]._bound = 1;
      ipa4 = _v[i]._ip4;
      //click_chatter("nat624::lookup -2");
      return (true);
    }
    else if (_v[i]._bound == 0 )
      {
	_v[i]._bound =1;
	ipa4=_v[i]._ip4;
	_v[i]._ip6 = ipa6;
	//click_chatter("nat624::lookup -3");
	return (true);
      }
  }

  return (false);
}
	
//make the translation of the packet according to SIIT (RFC 2765)
Packet *
Nat624::make_translate64(IPAddress src,
			 IPAddress dst,
			 unsigned short ip6_plen, 
			 unsigned char ip6_hlim, 
			 unsigned char ip6_nxt,
			 unsigned char *a,
			 unsigned char payload_length)
{
  click_ip *ip;
  unsigned char * b;
  WritablePacket *q = Packet::make(sizeof(*ip) + ntohs(ip6_plen));
  
  if (q==0) {
    click_chatter("can not make packet!");
    assert(0);
  }

  memset(q->data(), '\0', q->length());
  ip = (click_ip *)q->data();
  b = (unsigned char * ) (ip+1);
  
  //set ipv4 header
  ip->ip_v = 4;
  ip->ip_hl =5; 
  ip->ip_tos =0;
  ip->ip_len = htons(sizeof(*ip) + ntohs(ip6_plen));
  
  ip->ip_id = htons(0);

  //set Don't Fragment flag to true, all other flags to false, 
  //set fragement offset: 0
  ip->ip_off = htons(IP_DF);
  //we do not change the ttl since the packet has to go through v4 routing table
  ip->ip_ttl = ip6_hlim;

  if (ip6_nxt ==58) {
    ip->ip_p=1; 
  } else {
    ip->ip_p = ip6_nxt;
  }
 
  //set the src and dst address
  ip->ip_src = src.in_addr();
  ip->ip_dst = dst.in_addr();
 
  //copy the actual payload of packet
  memcpy(b, a, payload_length);
  //click_chatter("packet content b %x, %x, %x, %x, %x, %x", b[0], b[1], b[2], b[3], b[payload_length-2], b[payload_length-1]);

 //set the checksum
  ip->ip_sum=0;
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(click_ip));
  
  return q;	
		
}


Packet *
Nat624::make_icmp_translate64(unsigned char *a,
			      unsigned char payload_length)
{
  
  payload_length = payload_length - sizeof(click_ip6) + sizeof(click_ip);
  click_ip6 *ip6=0;
  unsigned char *ip=0;
  unsigned char icmp6_type= a[0];
  unsigned char icmp6_code= a[1];
  unsigned int icmp6_pointer = (unsigned int)a[4];
  unsigned char icmp_length =36;
  WritablePacket *q2 = 0;

  // set type, code and length of ICMP packet and its embedded IPv6 header
  if ( (icmp6_type == ICMP6_ECHO_REQUEST ) || (icmp6_type == ICMP6_ECHO_REPLY )) 
    {
      icmp6_echo *icmp6 = (icmp6_echo *)a;
      ip6 = (click_ip6 *)(icmp6+1);
      //icmp_length=payload_length - sizeof(icmp6_echo) + sizeof(icmp_echo);
      q2 = Packet::make(icmp_length);
      memset(q2->data(), '\0', q2->length());
      icmp_echo *icmp = (icmp_echo *)q2->data();
      ip = (unsigned char *)(icmp+1);
      
      if (icmp6_type == ICMP6_ECHO_REQUEST ) { // icmp6_type ==128 
	
	icmp->icmp_type = ICMP_ECHO;                 // icmp_type =8  
      }
      else if (icmp6_type == ICMP6_ECHO_REPLY ) { // icmp6_type ==129 
	icmp->icmp_type = ICMP_ECHO_REPLY;              // icmp_type = 0   
      }
      icmp->icmp_cksum = in_cksum((unsigned char *)icmp, sizeof(icmp_echo));
    }

  else if (icmp6_type == ICMP6_DST_UNREACHABLE) 
    { // icmp6_type ==11 
      icmp6_dst_unreach *icmp6 = (icmp6_dst_unreach *)a;
      ip6 = (click_ip6 *)(icmp6+1);
      //icmp_length=payload_length - sizeof(icmp6_dst_unreach) + sizeof(icmp_unreach);
      q2 = Packet::make(icmp_length);
      memset(q2->data(), '\0', q2->length());
      icmp_unreach *icmp = (icmp_unreach *)q2->data();
      ip = (unsigned char *)(icmp+1);
      icmp->icmp_type = ICMP_DST_UNREACHABLE;             // icmp_type =3    
   
      if (icmp6_code == 0)
	icmp->icmp_code = 0;
      else if (icmp6_code == 1)
	icmp->icmp_code = 10;
      else if (icmp6_code == 2)
	icmp->icmp_code = 5;
      else if (icmp6_code == 3)
	icmp->icmp_code = 1;
      else if (icmp6_code == 4)
	icmp->icmp_code = 3;
      icmp->icmp_cksum = in_cksum((unsigned char *)icmp, sizeof(icmp_unreach));
    
    }

  else if (icmp6_type == ICMP6_PKT_TOOBIG ) //icmp6_type == 2
    {  
      icmp6_pkt_toobig *icmp6 = (icmp6_pkt_toobig*)a;
      ip6 = (click_ip6 *)(icmp6+1);
      //icmp_length=payload_length - sizeof(icmp6_pkt_toobig) + sizeof(icmp_unreach);
      q2 = Packet::make(icmp_length);
      memset(q2->data(), '\0', q2->length());
      icmp_unreach *icmp = (icmp_unreach *)q2->data();
      ip = (unsigned char *)(icmp+1);
      icmp->icmp_type = ICMP_DST_UNREACHABLE; //icmp_type = 3
      icmp->icmp_code = 4;
      icmp->icmp_cksum = in_cksum((unsigned char *)icmp, sizeof(icmp_unreach));
    
    }
  
  else if (icmp6_type ==ICMP6_TYPE_TIME_EXCEEDED ) // icmp6_type == 3 
    { 
      icmp6_time_exceeded *icmp6 = (icmp6_time_exceeded *)a;
      ip6 = (click_ip6 *)(icmp6+1);
      //icmp_length=payload_length - sizeof(icmp6_time_exceeded) + sizeof(icmp6_time_exceeded);
      q2 = Packet::make(icmp_length);
      memset(q2->data(), '\0', q2->length());
      icmp_exceeded *icmp = (icmp_exceeded *)q2->data();
      ip = (unsigned char *)(icmp+1);
      icmp->icmp_type =ICMP_TYPE_TIME_EXCEEDED;            //icmp_type = 11 
      icmp->icmp_code = icmp6_code;
      icmp->icmp_cksum = in_cksum((unsigned char *)icmp, sizeof(icmp_exceeded));
    }

  else if (icmp6_type == ICMP6_PARAMETER_PROBLEM) // icmp6_type == 4
    {  
      icmp6_param *icmp6 = (icmp6_param *)a;
      ip6=(click_ip6 *)(icmp6+1);
      if (icmp6_code ==2 || icmp6_code ==0) 
	{
	  icmp_length=payload_length - sizeof(icmp6_param) + sizeof(icmp_param);
	  q2 = Packet::make(icmp_length);
	  memset(q2->data(), '\0', q2->length());
	  icmp_param *icmp = (icmp_param *)q2->data();
	  ip = (unsigned char *)(icmp +1);
	  icmp->icmp_type = ICMP_PARAMETER_PROBLEM;         // icmp_type = 12 
	  icmp->icmp_code = 0;
	  if (icmp6_code ==2) {
	    icmp->pointer = -1;
	  }
	  else if(icmp6_code ==0)
	    {
	      if (icmp6_pointer == 0)
		icmp->pointer = 0;
	      else if (icmp6_pointer == 4) 
		icmp->pointer = 2;
	      else if (icmp6_pointer == 7)
		icmp->pointer = 8;
	      else if (icmp6_pointer == 6)
		icmp->pointer = 9;
	      else if (icmp6_pointer == 8) 
		icmp->pointer = 12;
	      else if (icmp6_pointer == 24)
		icmp->pointer = -1;
	    }
	  icmp->icmp_cksum = in_cksum((unsigned char *)icmp, sizeof(icmp_param));
	}
    }

  else if (icmp6_code ==1) 
    {
      //icmp_length=payload_length - sizeof(icmp6_param) + sizeof(icmp_unreach);
      q2 = Packet::make(icmp_length);
      memset(q2->data(), '\0', q2->length());
      icmp_unreach *icmp = (icmp_unreach *)q2->data();
      
      ip = (unsigned char *)(icmp +1);
      icmp->icmp_type = ICMP_DST_UNREACHABLE;           // icmp_type = 3 
      icmp->icmp_code = 2;
      icmp->icmp_cksum = in_cksum((unsigned char *)icmp, sizeof(icmp_unreach));
    }

    //translate the IP6 packet that embedded in ICMP6 header.
  Packet *q3=0;
  unsigned char * start_of_q3_content = (unsigned char *)(ip6 +1);
  click_chatter("the data that start_of_q3_content points to is %x, %x", start_of_q3_content[0], start_of_q3_content[1]);
  unsigned char ip4_dst[4];
  unsigned char ip4_src[4];
  IP6Address ip6_dst;
  IP6Address ip6_src;
  IPAddress ip4_dsta;
  IPAddress ip4_srca;
  ip6_dst = IP6Address(ip6->ip6_dst);
  ip6_src = IP6Address(ip6->ip6_src);
  if ((ip6_dst.get_ip4address(ip4_dst) || lookup(ip6_dst, ip4_dsta)) &&
      (ip6_src.get_ip4address(ip4_src) || lookup(ip6_src, ip4_srca)))   
    {
      if (ip6_dst.get_ip4address(ip4_dst))
	ip4_dsta = IPAddress(ip4_dst);
      if (ip6_src.get_ip4address(ip4_src))
	ip4_srca = IPAddress(ip4_src);
      click_chatter("nat624::make_icmp_translate64 - 2, ip6->ip6_plen is %x", ip6->ip6_plen);
      q3 = make_translate64(ip4_srca, ip4_dsta, ip6->ip6_plen, ip6->ip6_hlim, ip6->ip6_nxt, start_of_q3_content, ip6->ip6_plen);
      unsigned char * q3d = q3->data();
      click_chatter("nat624::make_icmp_translate64 - 1, %x, %x, %x, %x, %x, %x, %x, %x", q3d[0], q3d[1], q3d[2], q3d[3], q3d[4], q3d[5], q3d[6], q3d[7]);
      click_chatter("nat624::make_icmp_translate64 - 1, %x, %x, %x, %x, %x, %x, %x, %x", q3d[20], q3d[21], q3d[2], q3d[23], q3d[24], q3d[25], q3d[26], q3d[27]);
      
      click_chatter("nat624::make_icmp_translate64 - 2, q3's length is %x", q3->length());
      
    }
  else {
    click_chatter("no addresses available for translating");
  }
  
  memcpy(ip, q3->data(), 28);
  unsigned char * q3d = (unsigned char *)ip;
  click_chatter("nat624::make_icmp_translate64 - 1, %x, %x, %x, %x, %x, %x, %x, %x", ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
   click_chatter("nat624::make_icmp_translate64 - 1, %x, %x, %x, %x, %x, %x, %x, %x", ip[20], ip[21], ip[22], ip[23], ip[24], ip[25], ip[26], ip[27]);
  q3->kill();
  return q2;
}



Packet *
Nat624::simple_action(Packet *p)
{
  click_ip6 *ip6 = (click_ip6 *) p->data();
  unsigned char * start_of_p= (unsigned char *)(ip6+1);
  unsigned char pl_length = p->length() - sizeof(*ip6);
  IPAddress ipa_src;
  IPAddress ipa_dst; 

 
  Packet *q = 0;
  unsigned char ip4_dst[4];
  IP6Address ip6_dst;
  ip6_dst = IP6Address(ip6->ip6_dst);
  
  // get the corresponding ipv4 src and dst addresses
  if (ip6_dst.get_ip4address(ip4_dst)
      && lookup(IP6Address(ip6->ip6_src), ipa_src))
    {
      ipa_dst = IPAddress(ip4_dst);
      //translate protocol according to SIIT
      q = make_translate64(ipa_src, ipa_dst, ip6->ip6_plen, ip6->ip6_hlim, ip6->ip6_nxt, start_of_p, pl_length);
      if (ip6->ip6_nxt == 0x3a) {
	click_chatter("This is also a ICMP6 Packet!");
	click_ip * ip4= (click_ip *)q->data();
	unsigned char * icmp6 = (unsigned char *)(ip4+1);
	Packet *q2 = 0;
	q2 = make_icmp_translate64(icmp6, (q->length()-sizeof(click_ip)));
	WritablePacket *q3=Packet::make(sizeof(click_ip)+q2->length());
	memset(q3->data(), '\0', q3->length());
	click_ip *start_of_q3 = (click_ip *)q3->data();
	memcpy(start_of_q3, q->data(), sizeof(click_ip));
	unsigned char *start_of_icmp = (unsigned char *)(start_of_q3+1);
	memcpy(start_of_icmp, q2->data(), q2->length()); 
	
	p->kill();
	q->kill();
	q2->kill();
	return (q3);
	
      }
      else {
	p->kill();
	return(q);
      }
    }

}


EXPORT_ELEMENT(Nat624)

// generate Vector template instance
#include "vector.cc"
template class Vector<Nat624::Entry64>;
