/* Copyright 2015 Outscale SAS
 *
 * This file is part of Butterfly.
 *
 * Butterfly is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 *
 * Butterfly is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Butterfly.  If not, see <http://www.gnu.org/licenses/>.
 */

extern "C" {
#include <glib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <unistd.h>
}
#include <utility>
#include <thread>
#include <chrono>
#include <cstring>
#include "api/server/app.h"
#include "api/server/graph.h"

namespace {
void PgfakeDestroy(struct pg_brick *) {}

/**
 * Convert a VNI to a mutlicast IP
 * @param   vni vni integer to convert
 * @return  multicast IP
 */
uint32_t BuildMulticastIp4(uint32_t vni) {
    // Build mutlicast IP, CIDR: 224.0.0.0/4
    // (224.0.0.0 to 239.255.255.255)
    // 224 and 239 are already used.
    uint32_t multicast_ip = htonl(vni);
    reinterpret_cast<uint8_t *>(& multicast_ip)[0] = 230;
    return multicast_ip;
}

void BuildMulticastIp6(uint8_t *multicast_ip, uint32_t vni) {
    memset(multicast_ip, 0, 16);
    multicast_ip[0] = 0xff;
    multicast_ip[15] = reinterpret_cast<uint8_t *>(& vni)[0];
    multicast_ip[14] = reinterpret_cast<uint8_t *>(& vni)[1];
    multicast_ip[13] = reinterpret_cast<uint8_t *>(& vni)[2];
    multicast_ip[12] = reinterpret_cast<uint8_t *>(& vni)[3];
}

}  // namespace

Graph::Graph(void) {
    // Init rpc queue
    queue_ = g_async_queue_new();
    started = false;
}

Graph::~Graph(void) {
    Stop();
}

bool Graph::LinkAndStalk(Graph::BrickShrPtr westBrick,
                         Graph::BrickShrPtr eastBrick,
                         Graph::BrickShrPtr sniffer) {
    if (app::config.packet_trace) {
        if (pg_brick_chained_links(&app::pg_error, westBrick.get(),
                                   sniffer.get(), eastBrick.get()) < 0) {
            PG_ERROR_(app::pg_error);
            return false;
        }
    } else {
        if (pg_brick_link(westBrick.get(), eastBrick.get(),
                          &app::pg_error) < 0) {
            PG_ERROR_(app::pg_error);
            return false;
        }
    }
    return true;
}

void Graph::Stop() {
    struct RpcQueue *a;
    std::map<std::string, app::Nic>::iterator n_it;

    if (!started)
        return;

    // Remove all NICs
    for (n_it = app::model.nics.begin();
         n_it != app::model.nics.end();
         n_it++) {
        NicDel(n_it->second);
    }

    // Stop vhost
    vhost_stop();

    // Stop poller thread
    exit();
    pthread_join(poller_thread, NULL);

    // Empty and unref queue
    a = (struct RpcQueue *)g_async_queue_try_pop(queue_);
    while (a != NULL) {
        g_free(a);
        a = (struct RpcQueue *)g_async_queue_try_pop(queue_);
    }
    g_async_queue_unref(queue_);

    // Byby packetgraph
    vnis_.clear();
    pg_stop();
    app::DestroyCgroup();
    started = false;
}

bool Graph::Start(std::string dpdk_args) {
    struct ether_addr mac;
    uint32_t useless, nic_capa_tx;

    // Start packetgraph
    if (!app::PgStart(dpdk_args)) {
        return false;
    }

    // DPDK open log for us and we WANT our logs back !
    app::Log::Open();

    // Start Vhost
    vhost_start();

    // Create nic brick
    if (app::config.dpdk_port < 0) {
        app::log.Error("invalid DPDK port " +
                       std::to_string(app::config.dpdk_port));
        return false;
    }
    nic_ = BrickShrPtr(pg_nic_new_by_id(
        ("port-" + std::to_string(app::config.dpdk_port)).c_str(),
        app::config.dpdk_port, &app::pg_error), pg_brick_destroy);
    if (nic_.get() == NULL) {
        PG_WARNING_(app::pg_error);
        // Try to create a pcap interface instead
        nic_ = BrickShrPtr(pg_tap_new("tap", NULL, &app::pg_error),
                     pg_brick_destroy);
        if (nic_.get() == NULL) {
            LOG_ERROR_("cannot create tap interface");
            PG_ERROR_(app::pg_error);
            return false;
        } else if (pg_tap_get_mac(nic_.get(), &mac) < 0) {
             LOG_ERROR_("cannot get mac of tap interface");
             return false;
        } else {
            LOG_INFO_("created tap interface %s", pg_tap_ifname(nic_.get()));
        }
    } else {
        app::log.Debug("using dpdk port " +
                       std::to_string(app::config.dpdk_port));
        SetConfigMtu();
        pg_nic_get_mac(nic_.get(), &mac);
    }
    pg_nic_capabilities(nic_.get(), &useless, &nic_capa_tx);
    if (app::config.no_offload ||
        !(nic_capa_tx & PG_NIC_TX_OFFLOAD_OUTER_IPV4_CKSUM) ||
        !(nic_capa_tx & PG_NIC_TX_OFFLOAD_TCP_TSO)) {
        if (app::config.no_offload)
            app::log.Info("offloading manually desactivated");
        else
            app::log.Info("no offloading available");
        pg_vhost_global_disable(VIRTIO_NET_F_HOST_TSO4 |
                                VIRTIO_NET_F_HOST_TSO6);
    } else {
        app::log.Info("some offloading is available");
    }

    // Create sniffer brick
    if (app::config.packet_trace) {
        pcap_file_ = fopen(("/tmp/butterfly-" + std::to_string(getpid()) +
                            "-main.pcap").c_str(), "w");
        std::string sniffer_name = "main-sniffer-" + std::to_string(getpid());
        sniffer_ = BrickShrPtr(pg_print_new(sniffer_name.c_str(), pcap_file_,
                               PG_PRINT_FLAG_PCAP | PG_PRINT_FLAG_CLOSE_FILE,
                               NULL, &app::pg_error),
                    pg_brick_destroy);
        if (sniffer_.get() == NULL) {
            PG_ERROR_(app::pg_error);
            return false;
        }
    }

    // Create vtep brick
    vtep_ = BrickShrPtr(pg_vtep_new_by_string("vxlan", 50, PG_WEST_SIDE,
                                              app::config.external_ip.c_str(),
                                              mac, PG_VTEP_DST_PORT,
                                              PG_VTEP_ALL_OPTI, &app::pg_error),
                        pg_brick_destroy);
    isVtep6_ = !strcmp(pg_brick_type(vtep_.get()), "vtep6");
    if (vtep_.get() == NULL) {
        PG_ERROR_(app::pg_error);
        return false;
    }

    LinkAndStalk(nic_, vtep_, sniffer_);

    // Run poller
    pthread_create(&poller_thread, NULL, Graph::Poller, this);

    started = true;
    return true;
}

void Graph::SetConfigMtu() {
    if (app::config.nic_mtu.length() == 0)
        goto exit;

    if (app::config.nic_mtu == "max") {
        app::log.Info("try to find maximal MTU");
        int min = 1400;
        int max = 65536;

        while (min != max - 1) {
            int m = (min + max) / 2;
            if (pg_nic_set_mtu(nic_.get(), m , &app::pg_error) < 0) {
                PG_ERROR_SILENT_(app::pg_error);
                max = m;
            } else {
                min = m;
            }
        }
        if (pg_nic_set_mtu(nic_.get(), min, &app::pg_error) < 0) {
            PG_ERROR_(app::pg_error);
            app::log.Error("failed to find minimal supported MTU");
        } else {
           app::log.Info("found maximal MTU of " + std::to_string(min));
        }
    } else {
        try {
            int mtu = std::stoi(app::config.nic_mtu);
            if (mtu > 0) {
                if (pg_nic_set_mtu(nic_.get(), mtu, &app::pg_error) < 0) {
                    PG_ERROR_(app::pg_error);
                } else {
                    app::log.Info("MTU successfully set to " +
                                  app::config.nic_mtu);
                }
            } else {
                LOG_ERROR_("bad MTU, must be > 0");
            }
        } catch(...) {
            app::log.Error("bad nic-mtu argument");
        }
    }

exit:
        uint16_t mtu;
        if (pg_nic_get_mtu(nic_.get(), &mtu, &app::pg_error) < 0) {
            PG_ERROR_SILENT_(app::pg_error);
            app::log.Debug("cannot get physical nic mtu");
        } else {
            app::log.Debug("physical nic mtu is " + std::to_string(mtu));
        }
}

#define POLLER_CHECK(c) (!((c) & 1023))
#define FIREWALL_GC(c) ((c) == 100000)
void *Graph::Poller(void *graph) {
    Graph *g = reinterpret_cast<Graph *>(graph);
    struct RpcUpdatePoll *list = NULL;
    struct RpcQueue *q = NULL;
    uint16_t pkts_count;
    struct pg_brick *nic = g->nic_.get();
    uint32_t size = 0;

    g_async_queue_ref(g->queue_);

    // Set CPU affinity for packetgraph processing
    Graph::SetCpu(app::config.graph_core_id);
    Graph::SetSched();

    /* The main packet poll loop. */
    for (uint32_t cnt = 0;; ++cnt) {
        /* Let's see if there is any update every 100 000 pools. */

        if (POLLER_CHECK(cnt)) {
            if (g->PollerUpdate(&q)) {
                if (q) {
                list = &q->update_poll;
                size = list->size;
                }
            } else {
                LOG_DEBUG_("poll thread will now exit");
                break;
            }
        }

        /* Poll all pollable vhosts. */
        if (pg_brick_poll(nic, &pkts_count, &app::pg_error) < 0)
            PG_ERROR_(app::pg_error);
        for (uint32_t v = 0; v < size; v++) {
            if (pg_brick_poll(list->pollables[v],
                              &pkts_count, &app::pg_error) < 0) {
                PG_ERROR_(app::pg_error);
            }
        }

        /* Call firewall garbage callector. */
        if (FIREWALL_GC(cnt)) {
            cnt = 0;
            for (uint32_t v = 0; v < size; v++)
                pg_firewall_gc(list->firewalls[v]);
            usleep(5);
        }
    }
    g_async_queue_unref(g->queue_);
    g_free(q);
    pthread_exit(NULL);
}
#undef POLLER_CHECK
#undef FIREWALL_GC

int Graph::SetCpu(int core_id) {
    cpu_set_t cpu_set;
    pthread_t t;

    if (core_id < 0 || core_id >= get_nprocs())
        return EINVAL;

    t = pthread_self();
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    return pthread_setaffinity_np(t, sizeof(cpu_set_t), &cpu_set);
}


#define gettid() syscall(SYS_gettid)

int Graph::SetSched() {
  app::config.tid = gettid();
  return 0;
}
#undef gettid

bool Graph::PollerUpdate(struct RpcQueue **list) {
    struct RpcQueue *a;
    struct RpcQueue *tmp;

    // Unqueue calls
    a = (struct RpcQueue *) g_async_queue_try_pop(queue_);
    while (a != NULL) {
        switch (a->action) {
            case EXIT:
                g_free(a);
                return false;
            case VHOST_START:
                if (pg_vhost_start(app::config.socket_folder.c_str(),
                                   &app::pg_error) < 0) {
                    PG_ERROR_(app::pg_error);
                }
                break;
            case VHOST_STOP:
                pg_vhost_stop();
                break;
            case LINK:
                if (pg_brick_link(a->link.w, a->link.e, &app::pg_error) < 0)
                    PG_ERROR_(app::pg_error);
                break;
            case UNLINK:
                pg_brick_unlink(a->unlink.b, &app::pg_error);
                if (pg_error_is_set(&app::pg_error))
                    PG_ERROR_(app::pg_error);
                break;
            case UNLINK_EDGE:
                pg_brick_unlink_edge(a->unlink_edge.w, a->unlink_edge.e,
                                    &app::pg_error);
                if (pg_error_is_set(&app::pg_error))
                    PG_ERROR_(app::pg_error);
                break;
            case ADD_VNI:
                if (isVtep6_) {
                    if (pg_vtep_add_vni(a->add_vni.vtep,
                                        a->add_vni.neighbor,
                                        a->add_vni.vni,
                                        a->add_vni.multicast_ip6,
                                        &app::pg_error) < 0)
                        PG_ERROR_(app::pg_error);
                } else {
                    if (pg_vtep_add_vni(a->add_vni.vtep,
                                        a->add_vni.neighbor,
                                        a->add_vni.vni,
                                        a->add_vni.multicast_ip4,
                                        &app::pg_error) < 0)
                        PG_ERROR_(app::pg_error);
                }
                break;
            case UPDATE_POLL:
                // Swap with the old list
                tmp = a;
                a = *list;
                *list = tmp;
                break;
            case FW_RELOAD:
                if (pg_firewall_reload(a->fw_reload.firewall,
                                       &app::pg_error) < 0)
                    PG_ERROR_(app::pg_error);
                break;
            case FW_NEW:
                *(a->fw_new.result) = pg_firewall_new(a->fw_new.name,
                                                      a->fw_new.flags,
                                                      &app::pg_error);
                if (pg_error_is_set(&app::pg_error))
                    PG_ERROR_(app::pg_error);
                break;
            case BRICK_DESTROY:
                pg_brick_destroy(a->brick_destroy.b);
                break;
            case NOTHING:
                break;

            default:
                LOG_ERROR_("brick poller has wrong RPC value");
                break;
        }
        g_free(a);
        a = (struct RpcQueue *) g_async_queue_try_pop(queue_);
    }

    return true;
}

bool Graph::NicAdd(app::Nic *nic_) {
    app::Nic &nic = *nic_;
    std::string name;

    if (!started) {
        LOG_ERROR_("Graph has not been started");
        return false;
    }

    // Create VNI if it does not exists
    auto it = vnis_.find(nic.vni);
    if (it == vnis_.end()) {
        struct GraphVni v;
        v.vni = nic.vni;
        std::pair<uint32_t, struct GraphVni> p(nic.vni, v);
        vnis_.insert(p);
        it = vnis_.find(nic.vni);
        if (it == vnis_.end())
            return false;
    }
    struct GraphVni &vni = it->second;

    // Create vhost branch
    struct GraphNic gn;
    struct pg_brick *tmp_fw = NULL;

    gn.enable = true;
    gn.id = nic.id;
    name = "firewall-" + gn.id;
    fw_new(name.c_str(), 1, 1, PG_NO_CONN_WORKER, &tmp_fw);
    WaitEmptyQueue();
    if (tmp_fw == NULL) {
        LOG_ERROR_("Firewall creation failed");
        return false;
    }

    gn.firewall = BrickShrPtr(tmp_fw, PgfakeDestroy);
    name = "antispoof-" + gn.id;
    struct ether_addr mac;
    nic.mac.Bytes(mac.ether_addr_octet);
    gn.antispoof = BrickShrPtr(pg_antispoof_new(name.c_str(), PG_WEST_SIDE,
                                                &mac, &app::pg_error),
                         pg_brick_destroy);
    if (!gn.antispoof) {
        PG_ERROR_(app::pg_error);
        return false;
    }

    if (nic.ip_anti_spoof) {
        for (auto it = nic.ip_list.begin(); it != nic.ip_list.end(); it++) {
            uint32_t ip;
            inet_pton(AF_INET, it->Str().c_str(), &ip);
            if (pg_antispoof_arp_add(gn.antispoof.get(),
                                     ip, &app::pg_error) < 0)
                PG_ERROR_(app::pg_error);
        }
        pg_antispoof_arp_enable(gn.antispoof.get());
    }

    LOG_INFO_("new nic now !\n");
    if (nic.type == app::VHOST_USER_SERVER) {
        name = "vhost-" + gn.id;
        gn.vhost = BrickShrPtr(pg_vhost_new(name.c_str(), 0,
                                            &app::pg_error),
                               pg_brick_destroy);
    } else if (nic.type == app::TAP) {
        name = gn.id;
        gn.vhost = BrickShrPtr(pg_tap_new(name.c_str(), name.c_str(),
                                            &app::pg_error),
                               pg_brick_destroy);
        if (!gn.vhost) {
            PG_ERROR_(app::pg_error);
            return false;
        }
    } else {
        LOG_ERROR_("unknow vhost type");
        return false;
    }

    if (!gn.vhost) {
        PG_ERROR_(app::pg_error);
        return false;
    }
    if (nic.packet_trace) {
        name = "sniffer-" + gn.id;
        gn.packet_trace_path = nic.packet_trace_path;
        gn.pcap_file = fopen(gn.packet_trace_path.c_str(), "w");
        gn.sniffer = BrickShrPtr(pg_print_new(name.c_str(), gn.pcap_file,
                                 PG_PRINT_FLAG_PCAP |
                                 PG_PRINT_FLAG_CLOSE_FILE,
                                 NULL, &app::pg_error),
                       pg_brick_destroy);
        if (!gn.sniffer) {
            PG_ERROR_(app::pg_error);
            return false;
        }
    }

    // Build branch and set head
    if (nic.bypass_filtering) {
        if (nic.packet_trace) {
            gn.head = gn.sniffer;
            if (pg_brick_link(gn.sniffer.get(), gn.vhost.get(),
                              &app::pg_error) < 0) {
                PG_ERROR_(app::pg_error);
                return false;
            }
        } else {
            gn.head = gn.vhost;
        }
    } else {
        gn.head = gn.firewall;
        if (pg_brick_link(gn.firewall.get(),
                          gn.antispoof.get(), &app::pg_error) < 0) {
            PG_ERROR_(app::pg_error);
            return false;
        }
        LinkAndStalk(gn.antispoof, gn.vhost, gn.sniffer);
    }

    // Link branch to the vtep
    if (vni.nics.size() == 0) {
        // Link directly vtep to branch's head
        link(vtep_, gn.head);
        add_vni(vtep_, gn.head, nic.vni);
    } else if (vni.nics.size() == 1) {
        // We have to insert a switch
        // - unlink the first branch head from the graph
        // - link a new switch to the vtep
        // - add the vni on the vtep with the switch
        // - link the first branch head to the switch
        // - link the second branch head to the switch
        name = "switch-" + std::to_string(nic.vni);
        vni.sw = BrickShrPtr(pg_switch_new(name.c_str(), 1, 30, PG_EAST_SIDE,
                                           &app::pg_error), pg_brick_destroy);
        if (!vni.sw) {
            PG_ERROR_(app::pg_error);
            return false;
        }

        BrickShrPtr head1 = vni.nics.begin()->second.head;
        if (pg_brick_unlink_edge(vtep_.get(), head1.get(),
                                 &app::pg_error) < 0) {
            PG_ERROR_(app::pg_error);
            return false;
        }
        link(vtep_, vni.sw);
        add_vni(vtep_, vni.sw, nic.vni);
        link(vni.sw, head1);
        link(vni.sw, gn.head);
    } else {
        // Switch already exist, just link branch to the switch
        link(vni.sw, gn.head);
    }

    // Add branch to the list of NICs
    std::pair<std::string, struct GraphNic> p(nic.id, gn);
    vni.nics.insert(p);

    // Update the list of pollable bricks
    update_poll();

    // Reload the firewall configuration
    FwUpdate(nic);
    app::SetCgroup();

    const char *ret = NicPath(gn.vhost);
    nic.path = std::string(ret);
    return true;
}

const char *Graph::NicPath(BrickShrPtr nic) {
    struct pg_brick *b = nic.get();

    if (!strcmp(pg_brick_type(b), "vhost"))
        return pg_vhost_socket_path(b);
    else
        return pg_tap_ifname(b);
}


Graph::GraphNic *Graph::FindNic(const app::Nic &nic) {
    auto vni_it = app::graph.vnis_.find(nic.vni);
    if (vni_it == app::graph.vnis_.end()) {
        LOG_ERROR_("NIC id: " + nic.id + " in vni: " +
            std::to_string(nic.vni) + " don't seems to exist.");
        return NULL;
    }

    struct GraphVni &vni = vni_it->second;
    auto nic_it = vni.nics.find(nic.id);
    if (nic_it == vni.nics.end()) {
        LOG_ERROR_("NIC id: " + nic.id + " in vni: " +
            std::to_string(nic.vni) + " don't seems to exist in branch.");
        return NULL;
    }
    return &nic_it->second;
}

void Graph::NicDel(const app::Nic &nic) {
    if (!started) {
        LOG_ERROR_("Graph has not been started");
        return;
    }

    auto vni_it = app::graph.vnis_.find(nic.vni);
    if (vni_it == app::graph.vnis_.end()) {
        LOG_ERROR_("NIC id: " + nic.id + " in vni: " +
            std::to_string(nic.vni) + " don't seems to exist.");
        return;
    }
    struct GraphVni &vni = vni_it->second;

    auto nic_it = vni.nics.find(nic.id);
    if (nic_it == vni.nics.end()) {
        LOG_ERROR_("NIC id: " + nic.id + " in vni: " +
            std::to_string(nic.vni) + " don't seems to exist in branch.");
        return;
    }

    // Disable branch and update poller
    struct GraphNic &n = nic_it->second;
    n.enable = false;
    update_poll();

    // Disconnect branch from vtep or switch
    if (vni.nics.size() == 1) {
        // We should only have a branch head directly connected to vtep
        unlink(n.head);
    } else if (vni.nics.size() == 2) {
        // We have do:
        // - unlink the switch which unlink all branch heads.
        // - connect the other head to vtep
        // - re-add other head to vni
        // - destroy the switch
        auto it = vni.nics.begin();
        if (it->second.id == nic.id)
            it++;
        struct GraphNic &other = it->second;
        unlink(vni.sw);
        link(app::graph.vtep_, other.head);
        add_vni(vtep_, other.head, nic.vni);
        WaitEmptyQueue();
        vni.sw.reset();
    } else {
        // We just have to unlink branch head from the switch
        unlink(n.head);
    }

    // Delete firewall in the processing thread
    brick_destroy(n.firewall);

    // Wait that queue is done before removing bricks
    WaitEmptyQueue();
    vni.nics.erase(nic_it);

    // Remove empty vni
    if (vni.nics.empty())
        vnis_.erase(vni.vni);
}

std::string Graph::NicExport(const app::Nic &nic) {
    if (!started) {
        LOG_ERROR_("Graph has not been started");
        return "";
    }

    // TODO(jerome.jutteau)
    std::string data = "";
    return data;
}

void Graph::NicGetStats(const app::Nic &nic, uint64_t *in, uint64_t *out) {
    *in = *out = 0;
    Graph::GraphNic *graph_nic = FindNic(nic);
    if (graph_nic == NULL)
        return;
    *in = pg_brick_rx_bytes(graph_nic->vhost.get());
    *out = pg_brick_tx_bytes(graph_nic->vhost.get());
}

void Graph::NicConfigAntiSpoof(const app::Nic &nic, bool enable) {
    Graph::GraphNic *graph_nic = FindNic(nic);
    if (graph_nic == NULL)
        return;

    BrickShrPtr &antispoof = graph_nic->antispoof;
    if (enable) {
        pg_antispoof_arp_del_all(antispoof.get());
        for (auto it = nic.ip_list.begin(); it != nic.ip_list.end(); it++) {
            uint32_t ip;
            inet_pton(AF_INET, it->Str().c_str(), &ip);
            if (pg_antispoof_arp_add(antispoof.get(),
                                     ip, &app::pg_error) < 0)
                PG_ERROR_(app::pg_error);
        }
        pg_antispoof_arp_enable(antispoof.get());
    } else {
        pg_antispoof_arp_disable(antispoof.get());
    }
}

void Graph::LinkSniffer(const app::Nic &nic, Graph::BrickShrPtr n_sniffer) {
    Graph::GraphNic *g_nic = FindNic(nic);

    if (nic.bypass_filtering) {
        unlink(g_nic->vhost);
        g_nic->head = n_sniffer;
        link(n_sniffer, g_nic->vhost);
        link(vtep_, g_nic->head);
        add_vni(vtep_, g_nic->head, nic.vni);
    } else {
        unlink_edge(g_nic->antispoof, g_nic->vhost);
        g_nic->head = n_sniffer;
        link(g_nic->antispoof, n_sniffer);
        link(n_sniffer, g_nic->vhost);
    }
}

void Graph::EnablePacketTrace(const app::Nic &nic) {
    Graph::GraphNic *g_nic = FindNic(nic);
    std::string name;
    if (nic.packet_trace) {
        app::log.Info("packet trace option on %s is already enabled",
                      nic.id.c_str());
        return;
    }

    if (g_nic->sniffer == NULL) {
        name = "sniffer-" + g_nic->id;
        g_nic->pcap_file = fopen(nic.packet_trace_path.c_str(), "w");
        g_nic->sniffer = BrickShrPtr(pg_print_new(name.c_str(),
                                 g_nic->pcap_file, PG_PRINT_FLAG_PCAP |
                                 PG_PRINT_FLAG_CLOSE_FILE,
                                 NULL, &app::pg_error),
                       pg_brick_destroy);
        if (!g_nic->sniffer) {
            PG_ERROR_(app::pg_error);
            return;
        }
    }
    LinkSniffer(nic, g_nic->sniffer);
}

void Graph::DisablePacketTrace(const app::Nic &nic) {
    Graph::GraphNic *g_nic = FindNic(nic);
    if (!nic.packet_trace) {
        app::log.Info("packet trace option on %s is already disabled",
                      nic.id.c_str());
        return;
    }

    if (g_nic->sniffer == NULL) {
        app::log.Error("can not find pcap brick");
        return;
    }

    if (nic.bypass_filtering) {
        unlink(g_nic->sniffer);
        g_nic->head = g_nic->vhost;
        link(vtep_, g_nic->head);
        add_vni(vtep_, g_nic->head, nic.vni);
    } else {
        unlink(g_nic->sniffer);
        g_nic->head = g_nic->antispoof;
        link(g_nic->antispoof, g_nic->vhost);
    }
}

void Graph::NicConfigPacketTrace(const app::Nic &nic, bool is_trace_set) {
    if (is_trace_set)
        EnablePacketTrace(nic);
    else
        DisablePacketTrace(nic);
    update_poll();
}

void Graph::NicConfigPacketTracePath(const app::Nic &nic,
                                     std::string update_path) {
    Graph::GraphNic *g_nic = FindNic(nic);
    FILE *n_pcap_file;
    std::string name;
    if (nic.packet_trace_path == update_path) {
        app::log.Info("packet trace path %s is already exist",
                      update_path.c_str());
         return;
    }

    DisablePacketTrace(nic);
    name = "sniffer-" + g_nic->id;
    n_pcap_file = fopen(update_path.c_str(), "w");
    BrickShrPtr n_sniffer = BrickShrPtr(pg_print_new(name.c_str(),
                                  n_pcap_file, PG_PRINT_FLAG_PCAP |
                                  PG_PRINT_FLAG_CLOSE_FILE,
                                  NULL, &app::pg_error), pg_brick_destroy);
    if (!n_sniffer) {
        PG_ERROR_(app::pg_error);
        return;
    }
    LinkSniffer(nic, n_sniffer);
    update_poll();
}

std::string Graph::FwBuildRule(const app::Rule &rule) {
    // Note that we only take into account inbound rules
    if (rule.direction == app::Rule::OUTBOUND)
        return "";

    std::string r;

    // Build source
    if (rule.security_group.length() == 0) {
        if (rule.cidr.mask_size != 0) {
            r += "src net " + rule.cidr.address.Str() +
                 "/" +  std::to_string(rule.cidr.mask_size);
        } else if (rule.cidr.address.Type() == app::Ip::V4) {
            r += "ip";
        } else {
            r += "ip6";
        }
    } else {
        auto sg = app::model.security_groups.find(rule.security_group);
        if (sg == app::model.security_groups.end()) {
            std::string m = "security group " + rule.security_group +
                            " not available";
            app::log.Error(m);
            return "";
        }
        if (sg->second.members.size() > 0) {
            r += " (";
            for (auto ip = sg->second.members.begin();
                 ip != sg->second.members.end();) {
                r += " src host " + ip->Str();
                if (++ip != sg->second.members.end())
                    r += " or";
            }
            r += ")";
        } else {
            std::string m = "no member in security group " + sg->second.id;
            app::log.Warning(m);
            return "";
        }
    }

    // Build protocol part
    switch (rule.protocol) {
    case IPPROTO_ICMP:
        r += " and icmp";
        break;
    case IPPROTO_ICMPV6:
        r += " and icmp6";
        break;
    case IPPROTO_TCP:
        r += " and tcp";
        break;
    case IPPROTO_UDP:
        r += " and udp";
        break;
    case -1:
        // Allow all
        break;
    default:
        // Note: this rule match first ipv6 header, not the potential next ones
        std::string p = std::to_string(rule.protocol);
        if (rule.cidr.address.Type() == app::Ip::V4) {
            r += " and (ip proto " + p + ")";
        } else {
            r += " and (ip6 proto " + p + ")";
        }
    }

    if (rule.protocol == IPPROTO_TCP || rule.protocol == IPPROTO_UDP) {
        if (rule.port_start < 65536 && rule.port_end < 65536) {
            if (rule.port_start == rule.port_end) {
                r += " dst port " + std::to_string(rule.port_end);
            } else if (rule.port_start < rule.port_end) {
                r += " dst portrange " + std::to_string(rule.port_start) +
                     "-" + std::to_string(rule.port_end);
            } else {
                LOG_ERROR_("invalid port range");
                return "";
            }
        } else {
            LOG_ERROR_("invalid port range");
            return "";
        }
    }

    return r;
}

std::string Graph::FwBuildSg(const app::Sg &sg) {
    std::string r;
    for (auto it = sg.rules.begin(); it != sg.rules.end();) {
        std::string fw_rule = FwBuildRule(it->second);
        if (fw_rule.length() == 0) {
            it++;
            continue;
        }
        r += "(" + fw_rule + ")";
        if (++it != sg.rules.end())
            r += "||";
    }
    // Special case when last rule is empty
    if (r.back() == '|') {
        r.pop_back();
        r.pop_back();
    }
    return r;
}

void Graph::FwUpdate(const app::Nic &nic) {
    if (!started) {
        LOG_ERROR_("Graph has not been started");
        return;
    }

    if (nic.bypass_filtering) {
        LOG_WARNING_("%s: skip firewall update when bypass filtering is on",
                     nic.id.c_str());
        return;
    }

    // Get firewall brick
    auto itvni = vnis_.find(nic.vni);
    if (itvni == vnis_.end())
        return;
    auto itnic = itvni->second.nics.find(nic.id);
    if (itnic == itvni->second.nics.end())
        return;
    BrickShrPtr &fw = itnic->second.firewall;

    // For each security groups, build rules inside a BIG one
    std::string in_rules;
    for (auto it = nic.security_groups.begin();
          it != nic.security_groups.end();) {
        auto sit = app::model.security_groups.find(*it);
        if (sit == app::model.security_groups.end()) {
            it++;
            continue;
        }
        std::string sg_rules = FwBuildSg(sit->second);
        if (sg_rules.length() == 0) {
            it++;
            continue;
        }

        in_rules += "(" + FwBuildSg(sit->second) + ")";
        if (++it != nic.security_groups.end())
            in_rules += "||";
    }
    // Special case when last rule is empty
    if (in_rules.back() == '|') {
        in_rules.pop_back();
        in_rules.pop_back();
    }

    // Set rules for the outgoing traffic: allow NIC's IPs
    std::string out_rules;
    for (auto it = nic.ip_list.begin(); it != nic.ip_list.end();) {
        out_rules += "(src host " + it->Str() + ")";
        if (++it != nic.ip_list.end())
            out_rules += " || ";
    }

    // Allow DHCP to exit
    // FIXME jerome.jutteau@outscale.com
    // This will be removed with OUTBOUND direction support.
    if (out_rules.length() > 0) {
        out_rules += " || ";
    }
    out_rules += "(src host 0.0.0.0 and dst host 255.255.255.255 and "
                 "udp src port 68 and udp dst port 67)";

    // Push rules to the firewall
    pg_firewall_rule_flush(fw.get());
    std::string m;
    m = "rules (in) for nic " + nic.id + ": " + in_rules;
    app::log.Debug(m);
    m = "rules (out) for nic " + nic.id + ": " + out_rules;
    app::log.Debug(m);
    if (in_rules.length() > 0 &&
        (pg_firewall_rule_add(fw.get(), in_rules.c_str(), PG_WEST_SIDE,
                              0, &app::pg_error) < 0)) {
        std::string m = "cannot build rules (in) for nic " + nic.id;
        app::log.Error(m);
        PG_ERROR_(app::pg_error);
        return;
    }
    if (out_rules.length() > 0 &&
        pg_firewall_rule_add(fw.get(), out_rules.c_str(), PG_EAST_SIDE,
                             1,  &app::pg_error) < 0) {
        std::string m = "cannot build rules (out) for nic " + nic.id;
        app::log.Error(m);
        PG_ERROR_(app::pg_error);
        return;
    }

    // Reload firewall
    fw_reload(fw);
}

void Graph::FwAddRule(const app::Nic &nic, const app::Rule &rule) {
    std::string m;
    if (!started) {
        LOG_ERROR_("Graph has not been started");
        return;
    }

    if (nic.bypass_filtering) {
        LOG_WARNING_("%s: add rule skipped, bypass filtering is on",
                     nic.id.c_str());
        return;
    }

    std::string r = FwBuildRule(rule);
    if (r.length() == 0) {
        m = "cannot build rule (add) for nic " + nic.id;
        app::log.Error(m);
        PG_ERROR_(app::pg_error);
        return;
    }

    m = "adding new rule to firewall of nic " + nic.id + ": " + r;
    app::log.Debug(m);

    // Get firewall brick
    auto itvni = vnis_.find(nic.vni);
    if (itvni == vnis_.end()) {
        LOG_ERROR_("cannot find nic's firewall");
        return;
    }
    auto itnic = itvni->second.nics.find(nic.id);
    if (itnic == itvni->second.nics.end()) {
        m = "nic " + nic.id + " not found";
        app::log.Error(m);
        return;
    }
    BrickShrPtr &fw = itnic->second.firewall;

    // Add rule & reload firewall
    if (pg_firewall_rule_add(fw.get(), r.c_str(), PG_WEST_SIDE,
                             0, &app::pg_error) < 0) {
        m = "cannot load rule (add) for nic " + nic.id;
        app::log.Error(m);
        app::log.Debug(r);
        return;
    }
    fw_reload(fw);
}

std::string Graph::Dot() {
    // Build the graph from the physical NIC
    return app::GraphDot(nic_.get());
}

void Graph::exit() {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = EXIT;
    g_async_queue_push(queue_, a);
}

void Graph::vhost_start() {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = VHOST_START;
    g_async_queue_push(queue_, a);
}

void Graph::vhost_stop() {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = VHOST_STOP;
    g_async_queue_push(queue_, a);
}

void Graph::link(BrickShrPtr w, BrickShrPtr e) {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = LINK;
    a->link.w = w.get();
    a->link.e = e.get();
    g_async_queue_push(queue_, a);
}

void Graph::unlink(BrickShrPtr b) {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = UNLINK;
    a->unlink.b = b.get();
    g_async_queue_push(queue_, a);
}

void Graph::unlink_edge(BrickShrPtr w, BrickShrPtr e) {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = UNLINK_EDGE;
    a->unlink_edge.w = w.get();
    a->unlink_edge.e = e.get();
    g_async_queue_push(queue_, a);
}

void Graph::fw_reload(BrickShrPtr b) {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = FW_RELOAD;
    a->fw_reload.firewall = b.get();
    g_async_queue_push(queue_, a);
}

void Graph::fw_new(const char *name,
                   uint32_t west_max,
                   uint32_t east_max,
                   uint64_t flags,
                   struct pg_brick **result) {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = FW_NEW;
    a->fw_new.name = name;
    a->fw_new.west_max = west_max;
    a->fw_new.east_max = east_max;
    a->fw_new.flags = flags;
    a->fw_new.result = result;
    g_async_queue_push(queue_, a);
}

void Graph::nothing_new() {
    struct RpcQueue *n = g_new(struct RpcQueue, 1);
    n->action = NOTHING;
    g_async_queue_push(queue_, n);
}

void Graph::brick_destroy(BrickShrPtr b) {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = BRICK_DESTROY;
    a->brick_destroy.b = b.get();
    g_async_queue_push(queue_, a);
}

void Graph::add_vni(BrickShrPtr vtep, BrickShrPtr neighbor, uint32_t vni) {
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    a->action = ADD_VNI;
    a->add_vni.vtep = vtep.get();
    a->add_vni.neighbor = neighbor.get();
    a->add_vni.vni = vni;
    if (!isVtep6_)
        a->add_vni.multicast_ip4 = BuildMulticastIp4(vni);
    else
        BuildMulticastIp6(a->add_vni.multicast_ip6, vni);
    g_async_queue_push(queue_, a);
}

void Graph::update_poll() {
    // Create a table with all pollable bricks
    std::map<uint32_t, struct GraphVni>::iterator vni_it;
    std::map<std::string, struct GraphNic>::iterator nic_it;
    struct RpcQueue *a = g_new(struct RpcQueue, 1);
    struct RpcUpdatePoll &p = a->update_poll;

    a->action = UPDATE_POLL;
    // Add physical NIC brick
    p.size = 0;
    // Add all vhost bricks
    for (vni_it = vnis_.begin();
            vni_it != vnis_.end();
            vni_it++) {
        for (nic_it = vni_it->second.nics.begin();
                nic_it != vni_it->second.nics.end();
                nic_it ++) {
            if (p.size + 1 >= GRAPH_VHOST_MAX_SIZE) {
                LOG_ERROR_("Not enough pollable bricks slot available");
                break;
            }
            if (!nic_it->second.enable)
                continue;
            p.pollables[p.size] = nic_it->second.vhost.get();
            p.firewalls[p.size] = nic_it->second.firewall.get();
            p.size++;
        }
    }

    // Pass this new listing to packetgraph thread
    g_async_queue_push(queue_, a);
}

void Graph::WaitEmptyQueue() {
    nothing_new();
    while (g_async_queue_length_unlocked(queue_) > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
