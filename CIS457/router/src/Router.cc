#include "../include/router/Router.hpp"
#include "../include/router/TableLookup.hpp"
#include "../include/router/ARPHeader.hpp"
#include "../include/router/ETHHeader.hpp"
#include "../include/router/ICMPHeader.hpp"
#include "../include/router/IPHeader.hpp"
#include "../include/router/NetworkInterface.hpp"
#include "../include/router/Error.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <netinet/if_ether.h>
#include <netinet/ip_icmp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace router {
  ARPHeader* Router::build_arp_reply(
      struct ether_header *eh,
      struct ether_arp *arp_frame,
      unsigned char local_addr[6]
      ) {
    ARPHeader *r = new ARPHeader();
    // SOURCE MAC FORMAT
    r->ea.ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
    // SOURCE MAC LENGTH
    r->ea.ea_hdr.ar_hln = ETHER_ADDR_LEN;
    // TARGET MAC
    std::memcpy(r->ea.arp_tha, arp_frame->arp_sha, 6);
    // TARGET PROTOCOL
    std::memcpy(r->ea.arp_tpa, arp_frame->arp_spa, 4);
    // TARGET MAC
    std::memcpy(r->ea.arp_sha, local_addr, 6);
    // TARGET PROTOCOL ACCRESS
    std::memcpy(r->ea.arp_spa, arp_frame->arp_tpa, 4);
    // PROTOCOL
    r->ea.ea_hdr.ar_pro = htons(ETH_P_IP);
    // PROTOCOL LENGTH
    r->ea.ea_hdr.ar_pln = sizeof(in_addr_t);
    // OP
    r->ea.ea_hdr.ar_op = htons(ARPOP_REPLY);
    // ETHERNET HEADER
    r->eh = *eh;
    return r;
  }

  /*
 * Checksum calculation.
 * Taken from: https://github.com/kohler/ipsumdump/blob/master/libclick-2.1/libsrc/in_cksum.c
 */
  uint16_t Router::checksum(unsigned char *addr, int len) {
      int nleft = len;
      const uint16_t *w = (const uint16_t *)addr;
      uint32_t sum = 0;
      uint16_t answer = 0;

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
        *(unsigned char *)(&answer) = *(const unsigned char *)w ;
        sum += answer;
      }

      /* add back carry outs from top 16 bits to low 16 bits */
      sum = (sum & 0xffff) + (sum >> 16);
      sum += (sum >> 16);
      /* guaranteed now that the lower 16 bits of sum are correct */

      answer = ~sum;              /* truncate to 16 bits */
      return answer;
  }

  bool Router::host_in_lookup_table(std::string host, std::unordered_map<std::string, std::string> lookup_table) {
    for (auto it = lookup_table.begin(); it != lookup_table.end(); ++it) {
      if (it->first.compare(host) == 0) {
        return true;
      }
    }

    return false;
  }

  ARPHeader* Router::build_arp_request(struct ether_header *eh, struct ether_arp *rp_frame, const unsigned char hop_ip[4]) {
    ARPHeader *r = new ARPHeader();
    // SOURCE MAC FORMAT
    r->ea.ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
    // SET PROTOCOL
    r->ea.ea_hdr.ar_pro = htons(ETH_P_IP);

    // SET HARDWARE ADDRESS LENGTH
    r->ea.ea_hdr.ar_hln = ETHER_ADDR_LEN;
    // SET IP ADDRESS LENGTH
    r->ea.ea_hdr.ar_pln = sizeof(in_addr_t);
    // DELCARE AS AN ARP REQUEST
    r->ea.ea_hdr.ar_op = htons(ARPOP_REQUEST);

    unsigned char broadcast_addr[6];
    for (int i = 0; i < 6; ++i) {
      broadcast_addr[i] = 0xFF;
    }

    // SET SOURCE HARDWARE ADDRESS
    std::memcpy(r->ea.arp_sha, rp_frame->arp_sha, 6);
    // SET TARGET HARDWARE PROTOCOL
    std::memcpy(r->ea.arp_spa, rp_frame->arp_spa, 4);
    // SET TARGET HARDWARE ADDRESS TO ALL ZERO
    std::memcpy(r->ea.arp_tha, broadcast_addr, 6);
    // SET TARGET PROTOCOL ADDRESS AS THE HOP IP
    std::memcpy(r->ea.arp_tpa, hop_ip, 4);
    // COPY ETHER HEADER
    r->eh = *eh;
    // SETS ETHER TYPE TO ARP
    r->eh.ether_type = ntohs(2048);

   return r;
  }

  int Router::Start(std::string lookup) {
    // Load the relevant table to do lookup only for itself
    TableLookup router_lookup_table(lookup);

    int packet_socket;
    int i = 0;
    struct ifaddrs *ifaddr, *tmp;
    struct timeval timeout;
    std::vector<NetworkInterface> net_inefs;

    timeout.tv_sec = 0;
    timeout.tv_usec = 500;

    if (getifaddrs(&ifaddr) == -1) {
      std::cerr << "getifaddrs machine broke" << std::endl;
      return EXIT_FAILURE;
    }

    for (tmp = ifaddr; tmp != nullptr; tmp = tmp->ifa_next) {
      NetworkInterface net_if;
      if (tmp->ifa_addr->sa_family == AF_PACKET) {
        std::cout << "Interface: " << tmp->ifa_name << std::endl;

        if (!strncmp(&(tmp->ifa_name[3]), "eth", 3)) {
          //std::cout << "Creating socket on interface: " << tmp->ifa_name << std::endl;
          //Get our mac
          struct sockaddr_ll *local_mac = (struct sockaddr_ll*) tmp->ifa_addr;
          struct sockaddr_in *local_add = (struct sockaddr_in*) tmp->ifa_addr;

          packet_socket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));

          if (packet_socket < 0) {
            std::cerr << "socket machine broke [" << packet_socket << "]" << std::endl;
            return errno;
          }

          if (bind(packet_socket, tmp->ifa_addr, sizeof(struct sockaddr_ll)) == -1) {
            std::cerr << "bind machine broke" << std::endl;
          }

          setsockopt(packet_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

          net_if.name = tmp->ifa_name;
          net_if.descr = packet_socket;
          std::memcpy(&net_if.mac_addr, local_mac->sll_addr, 6);
          net_inefs.push_back(net_if);
          printf("%s mac addr: %i:%i:%i:%i:%i:%i\n", net_if.name, net_if.mac_addr[0], net_if.mac_addr[1], net_if.mac_addr[2], net_if.mac_addr[3], net_if.mac_addr[4], net_if.mac_addr[5]);
        }
      }
      if (tmp->ifa_addr->sa_family == AF_INET) {
        if (!strncmp(&(tmp->ifa_name[3]), "eth", 3)) {
          struct sockaddr_in *local_add = (struct sockaddr_in*) tmp->ifa_addr;
          std::memcpy(&net_inefs[i].ip_addr, &(local_add->sin_addr.s_addr), 4);
          printf("%s ip addr: %i.%i.%i.%i\n", net_inefs[i].name, net_inefs[i].ip_addr[0], net_inefs[i].ip_addr[1], net_inefs[i].ip_addr[2], net_inefs[i].ip_addr[3]);
          i++;
        }
      }
    }

    printf("Listening for packets on %d net_inefs\n", net_inefs.size());
    std::unordered_map<std::string, std::vector<std::string>> queue_map;

    std::unordered_map<std::string, std::string> mac_cache;

    while (1) {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      int fd_max = 0;

      for (int i = 0; i < net_inefs.size(); ++i) {
        if (net_inefs[i].descr > fd_max) {
          fd_max = net_inefs[i].descr;
        }
        FD_SET(net_inefs[i].descr, &read_fds);
      }

      int activity = select(fd_max + 1, &read_fds, NULL, NULL, &timeout);

      if (activity == -1) {
        printf("Unable to modify socket file descriptor.\n");
      }

      for (int i = 0; i < net_inefs.size(); ++i) {
        if (FD_ISSET(net_inefs[i].descr, &read_fds)) {
          char buf[1500], send_buffer[1500];
          struct sockaddr_ll recvaddr;
          struct ether_header *eh_incoming, *eh_outgoing;
          struct ether_arp *arp_frame;
          ARPHeader *rp_incoming, *rp_outgoing;
          IPHeader *ip_incoming;
          IPHeader *ip_outgoing;
          ICMPHeader *icmp_incoming;
          ICMPHeader *icmp_outgoing;
          socklen_t recvaddrlen = sizeof(struct sockaddr_ll);

          int n = recvfrom(net_inefs[i].descr, buf, 1500, 0, (sockaddr*) &recvaddr, &recvaddrlen);
          if (n < 0) {
            //std::cerr << "recvfrom machine broke" << std::endl;
            continue;
          }

          if (recvaddr.sll_pkttype == PACKET_OUTGOING) continue;

          eh_incoming = (ether_header*) buf;
          rp_incoming = (ARPHeader*) (buf + sizeof(ether_header));
          ip_incoming = (IPHeader*) (buf + sizeof(ether_header));
          arp_frame = (ether_arp*) (buf + 14);

          printf("\nIncoming packet from %i.%i.%i.%i\n", ip_incoming->src_ip[0], ip_incoming->src_ip[1], ip_incoming->src_ip[2], ip_incoming->src_ip[3]);

          // Build the IP string for comparing later on
          std::string src_ip = std::to_string(ip_incoming->src_ip[0]) + "." +
            std::to_string(ip_incoming->src_ip[1]) + "." +
            std::to_string(ip_incoming->src_ip[2]) + "." +
            std::to_string(ip_incoming->src_ip[3]);

          std::string dest_ip = std::to_string(ip_incoming->dest_ip[0]) +"." +
            std::to_string(ip_incoming->dest_ip[1]) + "." +
            std::to_string(ip_incoming->dest_ip[2]) + "." +
            std::to_string(ip_incoming->dest_ip[3]);

          std::string fwd_inef = router_lookup_table.get_route(dest_ip);

          //std::cout << "SOURCE: " << src_ip << std::endl;
          //std::cout << "DESTINATION: " << dest_ip << std::endl;

          if (fwd_inef.length() > 0) {
            std::cout << "Found " << dest_ip << " in routing table. Forwaring to " << fwd_inef << std::endl;
          }

          eh_incoming->ether_type = ntohs(eh_incoming->ether_type);

          //If ARP request handled, build an arp reply
          if (eh_incoming->ether_type == ETHERTYPE_ARP) {
            if (ntohs(arp_frame->ea_hdr.ar_op) == 1) {
            std::string arp_src = std::to_string(arp_frame->arp_spa[0]) + "." +
              std::to_string(arp_frame->arp_spa[1]) + "." +
              std::to_string(arp_frame->arp_spa[2]) + "." +
              std::to_string(arp_frame->arp_spa[3]);

            std::string arp_dst = std::to_string(arp_frame->arp_tpa[0]) + "." +
              std::to_string(arp_frame->arp_tpa[1]) + "." +
              std::to_string(arp_frame->arp_tpa[2]) + "." +
              std::to_string(arp_frame->arp_tpa[3]);

            std::cout << "ARP Source: " << arp_src << std::endl;
            std::cout << "ARP Destination: " << arp_dst << std::endl;

            if (std::memcmp(arp_frame->arp_tpa, net_inefs[i].ip_addr, 4) == 0) {
            std::cout << "It would appear that I am this packet's destination!" << std::endl;

            // Get the interface information
            NetworkInterface dest_inef;
            for (int j = 0; j < net_inefs.size(); ++j) {
                if (net_inefs[j].name == fwd_inef) {
              dest_inef = net_inefs[j];
              break;
              }
            }

            // Building arp reply here and storing into outgoing arp reply header
            rp_outgoing = build_arp_reply(eh_incoming, arp_frame, net_inefs[i].mac_addr);
            std::memcpy(send_buffer, rp_outgoing, 1500);

            // Move data into Ethernet struct too
            eh_outgoing = (ether_header*) send_buffer;
            std::memcpy(eh_outgoing->ether_dhost, eh_incoming->ether_shost, 6);
            std::memcpy(eh_outgoing->ether_shost, net_inefs[i].mac_addr, 6);
            eh_outgoing->ether_type = htons(0x0806);

            // Send the damn thing
            std::cout << "Sending ARP reply" << std::endl;

            if(send(net_inefs[i].descr, send_buffer, 42, 0) == -1) {
              std::cout << "Error sending arp reply" << std::endl;
            }
            }
            }
          } else if (eh_incoming->ether_type == ETHERTYPE_IP) {
            std::cout << "IP/ICMP packet found" << std::endl;
            std::memcpy(send_buffer, buf, 1500);
            icmp_incoming = (ICMPHeader*) (buf + 34);
            // If the host is in the lookup table we can just forward the packet like normal
            // Thisis from PART 2 BRANCH
            struct sockaddr_in arp_socket;
            std::memcpy(&arp_socket.sin_addr.s_addr, ip_incoming->dest_ip, 4);
            char* addr = inet_ntoa(arp_socket.sin_addr);
            std::string ip_dest = addr;
            std::string fwd_inef = router_lookup_table.get_route(ip_dest);
            std::string hop_dest = router_lookup_table.get_hop_device(ip_dest);
            std::cout << "Interface Forward: " << fwd_inef << " | Hop IP: " << hop_dest << std::endl;
            bool is_end_device = false;
            for (int j = 0; j < net_inefs.size(); ++j) {
              if (memcmp(ip_incoming->dest_ip, &net_inefs[j].ip_addr, 4) == 0) {
                is_end_device = true;
                break;
              }
            }

        if (is_end_device) {
          std::cout << "The requested ip is in the lookup table, forwarding normally" << std::endl;
          // Copy data into the ICMP header
          icmp_outgoing = (ICMPHeader*) (send_buffer + sizeof(ether_header) + sizeof(IPHeader));
          icmp_outgoing->type = 0;
          icmp_outgoing->checksum = 0;
          icmp_outgoing->checksum = checksum(reinterpret_cast<unsigned char*>(icmp_outgoing), (1500 - sizeof(ether_header) + sizeof(IPHeader)));

          ip_outgoing = (IPHeader*) (send_buffer + sizeof(ether_header));
          std::memcpy(ip_outgoing->src_ip, ip_incoming->dest_ip, 4);
          std::memcpy(ip_outgoing->dest_ip, ip_incoming->src_ip, 4);

          std::cout << "Building the ICMP ethernet header" << std::endl;
          eh_outgoing = (ether_header*) send_buffer;
          std::memcpy(eh_outgoing->ether_dhost, eh_incoming->ether_shost, 6);
          std::memcpy(eh_outgoing->ether_shost, net_inefs[i].mac_addr, 6);
          eh_outgoing->ether_type = htons(0x800);

          //std::string src_ip(reinterpret_cast<const char*>(ip_outgoing->src_ip), 6);
          //std::cout << src_ip << std::endl;
          if (send(net_inefs[i].descr, send_buffer, n, 0) == -1) {
            std::cout << "There was an error sending the ICMP echo packet" << std::endl;
          }
        } else {
            // If it's not in the table we need to send to the next router, but to do that we need to first ARP
            std::cout << "Beginning packet forward process" << std::endl;
	    ip_outgoing = (IPHeader*) (send_buffer + sizeof(ether_header));
	    ip_outgoing->ttl = ip_outgoing->ttl - 1;
	    router::Error err;
	    if ((int) ip_outgoing->ttl <= 0) {
	      // Send error if our TTL is 0
	      std::cout << "Packet TTL is 0. Sending error..." << std::endl;
	      icmp_outgoing = (ICMPHeader*) (send_buffer + sizeof(ether_header) + sizeof(IPHeader));
              icmp_outgoing->type = err.TYPE_TTL;
	      icmp_outgoing->code = err.CODE_ZERO;
              icmp_outgoing->checksum = 0;
              icmp_outgoing->checksum = checksum(reinterpret_cast<unsigned char*>(icmp_outgoing), (1500 - sizeof(ether_header) + sizeof(IPHeader)));

	      ip_outgoing = (IPHeader*) (send_buffer + sizeof(ether_header));
              std::memcpy(ip_outgoing->src_ip, ip_incoming->dest_ip, 4);
              std::memcpy(ip_outgoing->dest_ip, net_inefs[i].ip_addr, 4);

              eh_outgoing = (ether_header*) send_buffer;
              std::memcpy(eh_outgoing->ether_dhost, eh_incoming->ether_shost, 6);
              std::memcpy(eh_outgoing->ether_shost, net_inefs[i].mac_addr, 6);
              eh_outgoing->ether_type = htons(0x800);

	      if (send(net_inefs[i].descr, send_buffer, n, 0) == -1) {
    	        std::cout << "There was an error sending the ICMP echo packet" << std::endl;
              }
	    } else if (fwd_inef.length() <= 0) {
	      // Send error if there is no available forward interface
	      std::cout << "Network destination unreachable. Sending error..." << std::endl;
	      icmp_outgoing = (ICMPHeader*) (send_buffer + sizeof(ether_header) + sizeof(IPHeader));
              ip_outgoing->ttl = ip_outgoing->ttl + 1;
              icmp_outgoing->type = err.TYPE_UNREACHABLE;
	      icmp_outgoing->code = err.CODE_ZERO;
              icmp_outgoing->checksum = 0;
              icmp_outgoing->checksum = checksum(reinterpret_cast<unsigned char*>(icmp_outgoing), (1500 - sizeof(ether_header) + sizeof(IPHeader)));

	      ip_outgoing = (IPHeader*) (send_buffer + sizeof(ether_header));
	      ip_outgoing->checksum = 0;
              std::memcpy(ip_outgoing->src_ip, net_inefs[i].ip_addr, 4);
              std::memcpy(ip_outgoing->dest_ip, ip_incoming->src_ip, 4);
	      ip_outgoing->checksum = checksum(reinterpret_cast<unsigned char*>(ip_outgoing), 	(sizeof(IPHeader)));

              eh_outgoing = (ether_header*) send_buffer;
              std::memcpy(eh_outgoing->ether_dhost, eh_incoming->ether_shost, 6);
              std::memcpy(eh_outgoing->ether_shost, net_inefs[i].mac_addr, 6);
              eh_outgoing->ether_type = htons(0x800);

	      if (send(net_inefs[i].descr, send_buffer, n, 0) == -1) {
    	        std::cout << "There was an error sending the ICMP echo packet" << std::endl;
              }
	    } else {
	    ip_outgoing->checksum = 0;
	    ip_outgoing->checksum = checksum(reinterpret_cast<unsigned char*>(ip_outgoing), (sizeof(IPHeader)));
            std::cout << "IP TTL: " << (int) ip_outgoing->ttl << std::endl;
	    unsigned char buffer[98];
            std::memcpy(&buffer, &buf[0], 98);
	    // idk why this doesn't work
	    //uint16_t checksum_verify = checksum(buffer, (sizeof(IPHeader));
	    //std::cout << "IP Incoming Checksum: " << (int) ip_incoming->checksum << " | " << (int) checksum_verify << std::endl;

            unsigned char broadcast[6];
            for (int i = 0; i < 6; ++i)
              broadcast[i] = 0xFF;

            // Get the interface information
            NetworkInterface dest_inef;
            for (int j = 0; j < net_inefs.size(); ++j) {
              if (net_inefs[j].name == fwd_inef) {
                  dest_inef = net_inefs[j];
                  break;
              }
            }
            unsigned char target_ip[4];
            std::memcpy(target_ip, ip_incoming->dest_ip, 4);
            // Detect if there is a HOP address
            if (hop_dest.length() > 0) {
              // Convert HOP IP to IP address
              int length = hop_dest.length();
              char raw_ip[length];
              std::strcpy(raw_ip, hop_dest.c_str());
              inet_pton(AF_INET, raw_ip, &target_ip);
            }
	    
            char temp_buffer[1500];
	    char old_buffer[1500];
            char arp_buffer[42];
	    std::memcpy(old_buffer, send_buffer, 1500);

	    std::string string_target_ip = (std::string) reinterpret_cast<char*>(target_ip);
	    
	    // Check if we already know the MAC for this target_ip
	    if (mac_cache.find(string_target_ip) != mac_cache.end()) {

	      std::cout << "Cached mac found" << std::endl;
	      memcpy(arp_buffer, (mac_cache.find(string_target_ip)->second.c_str()), 42);

	    } else {

              //std::cout << "Building the arp request now for " << std::to_string(ip) << std::endl;
              rp_outgoing = build_arp_request(eh_incoming, arp_frame, target_ip);
              std::memcpy(rp_outgoing->ea.arp_spa, net_inefs[i].ip_addr, 4);
              std::memcpy(rp_outgoing->ea.arp_tpa, target_ip, 4);
              std::memcpy(rp_outgoing->ea.arp_tha, broadcast, 6);
              std::memcpy(rp_outgoing->ea.arp_sha, net_inefs[i].mac_addr, 6);
              std::memcpy(send_buffer, rp_outgoing, 1500);
              // This part may be the cause of some issues
              std::cout << "Building the ethernet header now for " << dest_inef.name << std::endl;
              eh_outgoing = (ether_header*) send_buffer;
              std::memcpy(eh_outgoing->ether_dhost, &broadcast, 6);
              std::memcpy(eh_outgoing->ether_shost, dest_inef.mac_addr, 6);
              eh_outgoing->ether_type = htons(0x0806);

                    // Issue might be line above this, eh_incoming->ether_dhost seems wrong...

              if (send(dest_inef.descr, send_buffer, 42, 0) == -1) {
                std::cout << "There was an error sending the ICMP echo packet" << std::endl;
              }

	      int res_bytes = 1;
              struct sockaddr_ll recvaddr;
              socklen_t sin_size = sizeof(sockaddr_ll);
              std::cout << "BLocking until we get the MAC back from " << dest_inef.name << std::endl;

              // Get ready to get the contents
              while(1) {
                res_bytes = recvfrom(dest_inef.descr, temp_buffer, 1500, 0, (sockaddr*) &recvaddr, &sin_size);
                if (recvaddr.sll_pkttype == PACKET_OUTGOING) continue;
                if (res_bytes < 0) { 
                  std::cout << "Timed out waiting for destination ARP reply" << std::endl;
		  break;
	        } else {
		  std::cout << "Got the mac" << std::endl;
		  break;
		}
              }

	      if (res_bytes < 0) {
		// Send error if there is no available forward interface
	        std::cout << "Network host unreachable. Sending error..." << std::endl;
	        icmp_outgoing = (ICMPHeader*) (old_buffer + sizeof(ether_header) + sizeof(IPHeader));
                ip_outgoing->ttl = ip_outgoing->ttl + 1;
                icmp_outgoing->type = err.TYPE_UNREACHABLE;
	        icmp_outgoing->code = err.CODE_ONE;
                icmp_outgoing->checksum = 0;
                icmp_outgoing->checksum = checksum(reinterpret_cast<unsigned char*>(icmp_outgoing), (1500 - sizeof(ether_header) + sizeof(IPHeader)));

                 ip_outgoing = (IPHeader*) (old_buffer + sizeof(ether_header));
                 ip_outgoing->checksum = 0;
                 std::memcpy(ip_outgoing->src_ip, dest_inef.ip_addr, 4);
                 std::memcpy(ip_outgoing->dest_ip, ip_incoming->src_ip, 4);
                 ip_outgoing->checksum = checksum(reinterpret_cast<unsigned char*>(ip_outgoing), 	(sizeof(IPHeader)));
                 eh_outgoing = (ether_header*) old_buffer;
                 std::memcpy(eh_outgoing->ether_dhost, eh_incoming->ether_shost, 6);
                 std::memcpy(eh_outgoing->ether_shost, dest_inef.mac_addr, 6);
                 eh_outgoing->ether_type = htons(0x800);
	         if (send(net_inefs[i].descr, old_buffer, n, 0) == -1) {
    	           std::cout << "There was an error sending the ICMP echo packet" << std::endl;
                 }
	         continue;
	      }

              std::memcpy(arp_buffer, temp_buffer, 42);

	      // Store the mac in cache
  	      mac_cache[string_target_ip] = (std::string) reinterpret_cast<char*>(arp_buffer);

	    }

            struct ether_header* eh_forward = (ether_header*) arp_buffer;
            std::memcpy(eh_forward->ether_dhost, eh_forward->ether_shost, 6);
            std::memcpy(eh_forward->ether_shost, dest_inef.mac_addr, 6);
            eh_forward->ether_type = htons(0x0800);

            std::memcpy(&buffer[0], eh_forward, 14);

            // Send here
            std::cout << "Sending ICMP response to " << dest_inef.name << std::endl;
            send(dest_inef.descr, buffer, sizeof(buffer), 0);
          }
	    }
        }
      }
    }
  }

    freeifaddrs(ifaddr);
    return EXIT_SUCCESS;
  }
} // namespace router
