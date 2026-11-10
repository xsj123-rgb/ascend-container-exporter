// ascend-container-exporter — 容器级 NPU 显存记账聚合器
//
// 消费 LD_PRELOAD shim (libnpu-account.so) 落盘的 JSONL 事件流
// (/run/npu-account/<container_id>.jsonl)，结合 K8s API 将 container_id
// 映射到 pod/namespace/container，输出 Prometheus 指标。
//
// 事件行（shim 保证每行完整写入）：
//   {"t":"I","pid":..,"cid":".."}                       进程 init（忽略，cid 在文件名）
//   {"t":"M","pid":..,"sym":"rtMalloc","key":"0x..","size":..}
//   {"t":"F","pid":..,"sym":"rtFree","key":"0x..","size":..}
//   {"t":"S","pid":..,"hpid":..,"live":..,"total":..,"freed":..}   每事件后快照
//   {"t":"X","pid":..,"hpid":..,"live":..,...}                      进程退出
//
// 聚合语义：每容器 = Σ 各存活 pid 的 S.live（绝对值快照，幂等重放安全）；
// pid 存活判定：宿主机扫 /proc/*/cgroup 的 cri-containerd-<cid> + NSpid 末位
// （容器内无法得知自身宿主 pid，/proc/self/sched 返回的也是 namespace pid）。
// 孤儿文件（容器已删）超过 gc-after 阈值后自动清理。
package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"
)

const version = "0.4.0"

// ---- 事件与状态 ----

type event struct {
	T     string `json:"t"`
	PID   int    `json:"pid"`
	HPID  int    `json:"hpid"`
	Live  uint64 `json:"live"`
	Total uint64 `json:"total"`
	Freed uint64 `json:"freed"`
}

type procState struct {
	hpid  int
	live  uint64
	total uint64
	freed uint64
}

// cid → pid → state（文件被截断/容器重启时整体重置）
type fileState struct {
	offset int64
	pids   map[int]*procState
}

type exporter struct {
	mu       sync.Mutex
	dir      string
	nodeName string
	files    map[string]*fileState // cid → state

	live, total, freed, pids *prometheus.GaugeVec
	orphans, parseErrs       prometheus.Gauge
	scrapeAge                prometheus.Gauge
}

func main() {
	var (
		dir       = flag.String("dir", "/run/npu-account", "记账事件流目录")
		listen    = flag.String("listen", ":9400", "HTTP 监听地址")
		nodeName  = flag.String("node", os.Getenv("NODE_NAME"), "节点名（默认取 NODE_NAME env / hostname）")
		kubecfg   = flag.String("kubeconfig", os.Getenv("KUBECONFIG"), "kubeconfig（缺省走 in-cluster）")
		interval  = flag.Duration("interval", 10*time.Second, "采集周期")
		gcAfter   = flag.Duration("gc-after", 30*time.Minute, "孤儿文件删除阈值（cid 不在 K8s 且 mtime 超过此值）")
	)
	flag.Parse()

	if *nodeName == "" {
		h, err := os.Hostname()
		if err != nil {
			log.Fatalf("node name: %v", err)
		}
		*nodeName = h
	}

	e := &exporter{
		dir:       *dir,
		nodeName:  *nodeName,
		files:     map[string]*fileState{},
	}
	e.live = prometheus.NewGaugeVec(prometheus.GaugeOpts{
		Name: "ascend_container_hbm_live_bytes",
		Help: "Per-container live NPU HBM bytes accounted by LD_PRELOAD shim (sum of live pids)",
	}, []string{"node", "namespace", "pod_name", "container_name"})
	e.total = prometheus.NewGaugeVec(prometheus.GaugeOpts{
		Name: "ascend_container_hbm_total_bytes",
		Help: "Cumulative allocated bytes per container",
	}, []string{"node", "namespace", "pod_name", "container_name"})
	e.freed = prometheus.NewGaugeVec(prometheus.GaugeOpts{
		Name: "ascend_container_hbm_freed_bytes",
		Help: "Cumulative freed bytes per container",
	}, []string{"node", "namespace", "pod_name", "container_name"})
	e.pids = prometheus.NewGaugeVec(prometheus.GaugeOpts{
		Name: "ascend_container_accounting_pids",
		Help: "Number of live accounted processes per container",
	}, []string{"node", "namespace", "pod_name", "container_name"})
	e.orphans = prometheus.NewGauge(prometheus.GaugeOpts{
		Name: "ascend_accounting_orphan_files",
		Help: "Accounting files whose container is not in K8s",
	})
	e.parseErrs = prometheus.NewGauge(prometheus.GaugeOpts{
		Name: "ascend_accounting_parse_errors_total",
		Help: "Cumulative unparsable lines",
	})
	e.scrapeAge = prometheus.NewGauge(prometheus.GaugeOpts{
		Name: "ascend_accounting_last_scrape_timestamp_seconds",
		Help: "Unix time of last successful scrape",
	})

	reg := prometheus.NewRegistry()
	reg.MustRegister(e.live, e.total, e.freed, e.pids, e.orphans, e.parseErrs, e.scrapeAge)
	prometheus.MustRegister(prometheus.NewGaugeFunc(prometheus.GaugeOpts{
		Name: "ascend_container_exporter_build_info",
		Help: "Build info",
		ConstLabels: prometheus.Labels{"version": version},
	}, func() float64 { return 1 }))

	k8s, err := newClient(*kubecfg)
	if err != nil {
		log.Fatalf("k8s client: %v", err)
	}

	go func() {
		var parseErrs uint64
		for {
			cidMap, err := listNodeContainers(k8s, *nodeName)
			if err != nil {
				log.Printf("list pods: %v", err)
			}
			alive := containerPidMap()
			e.scrape(cidMap, alive, *gcAfter, &parseErrs)
			e.parseErrs.Set(float64(parseErrs))
			e.scrapeAge.Set(float64(time.Now().Unix()))
			time.Sleep(*interval)
		}
	}()

	http.Handle("/metrics", promhttp.HandlerFor(reg, promhttp.HandlerOpts{}))
	http.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintln(w, "ok")
	})
	log.Printf("ascend-container-exporter v%s listening %s (node=%s dir=%s)", version, *listen, *nodeName, *dir)
	log.Fatal(http.ListenAndServe(*listen, nil))
}

// ---- K8s：container_id → pod 元数据 ----

type ctrMeta struct {
	ns, pod, container string
}

func newClient(kubecfg string) (*kubernetes.Clientset, error) {
	if kubecfg != "" {
		cfg, err := clientcmd.BuildConfigFromFlags("", kubecfg)
		if err != nil {
			return nil, err
		}
		return kubernetes.NewForConfig(cfg)
	}
	cfg, err := rest.InClusterConfig()
	if err != nil {
		return nil, err
	}
	return kubernetes.NewForConfig(cfg)
}

func listNodeContainers(c *kubernetes.Clientset, node string) (map[string]ctrMeta, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	pods, err := c.CoreV1().Pods("").List(ctx, metav1.ListOptions{
		FieldSelector: "spec.nodeName=" + node,
	})
	if err != nil {
		return nil, err
	}
	m := map[string]ctrMeta{}
	for _, p := range pods.Items {
		for _, st := range p.Status.ContainerStatuses {
			cid := strings.TrimPrefix(st.ContainerID, "containerd://")
			if cid == "" || cid == st.ContainerID { // 非 containerd
				continue
			}
			m[cid] = ctrMeta{ns: p.Namespace, pod: p.Name, container: st.Name}
		}
	}
	return m, nil
}

// ---- 宿主机 /proc 扫描：cid → 存活的容器内 pid 集合 ----
// 宿主机视角下 /proc/<hostpid>/cgroup 含 cri-containerd-<cid>.scope，
// /proc/<hostpid>/status 的 NSpid 末位即该进程的容器内 pid。
// 一次扫描同时解决「容器 pid → 存活判定」与「崩溃进程记账残留」。

func containerPidMap() map[string]map[int]bool {
	m := map[string]map[int]bool{}
	ents, err := os.ReadDir("/proc")
	if err != nil {
		return m
	}
	for _, ent := range ents {
		pid, err := strconv.Atoi(ent.Name())
		if err != nil {
			continue
		}
		p := "/proc/" + ent.Name()
		cg, err := os.ReadFile(p + "/cgroup")
		if err != nil {
			continue
		}
		cid := extractCid(string(cg))
		if cid == "" {
			continue
		}
		st, err := os.ReadFile(p + "/status")
		if err != nil {
			continue
		}
		nsPid := lastNSpid(st)
		if nsPid <= 0 {
			nsPid = pid
		}
		if m[cid] == nil {
			m[cid] = map[int]bool{}
		}
		m[cid][nsPid] = true
	}
	return m
}

func extractCid(cgroup string) string {
	for {
		i := strings.Index(cgroup, "cri-containerd-")
		if i < 0 {
			return ""
		}
		p := cgroup[i+len("cri-containerd-"):]
		if len(p) >= 65 {
			c := p[:64]
			ok := true
			for j := 0; j < 64; j++ {
				if !((c[j] >= '0' && c[j] <= '9') || (c[j] >= 'a' && c[j] <= 'f')) {
					ok = false
					break
				}
			}
			if ok && p[64] == '.' {
				return c
			}
		}
		cgroup = cgroup[i+len("cri-containerd-"):]
	}
}

func lastNSpid(status []byte) int {
	for _, line := range bytes.Split(status, []byte("\n")) {
		if !bytes.HasPrefix(line, []byte("NSpid:")) {
			continue
		}
		fields := bytes.Fields(line[len("NSpid:"):])
		if len(fields) == 0 {
			return 0
		}
		v, err := strconv.Atoi(string(fields[len(fields)-1]))
		if err != nil {
			return 0
		}
		return v
	}
	return 0
}

// ---- 采集主逻辑 ----

func (e *exporter) scrape(cidMap map[string]ctrMeta, alive map[string]map[int]bool, gcAfter time.Duration, parseErrs *uint64) {
	e.mu.Lock()
	defer e.mu.Unlock()

	// 1. tail 全部 *.jsonl
	ents, _ := os.ReadDir(e.dir)
	seen := map[string]bool{}
	for _, ent := range ents {
		name := ent.Name()
		if !strings.HasSuffix(name, ".jsonl") {
			continue
		}
		cid := strings.TrimSuffix(name, ".jsonl")
		seen[cid] = true
		if err := e.tailFile(cid); err != nil {
			log.Printf("tail %s: %v", name, err)
		}
	}
	// 文件消失（被 GC/删除）→ 清状态
	for cid := range e.files {
		if !seen[cid] {
			delete(e.files, cid)
		}
	}

	// 2. hpid 存活过滤 + 聚合
	type agg struct {
		live, total, freed uint64
		pids               int
	}
	aggBy := map[string]*agg{}
	labelsBy := map[string]prometheus.Labels{}
	orphans := 0
	for cid, fs := range e.files {
		var (
			live, total, freed uint64
			npid               int
		)
		for pid, st := range fs.pids {
			// 存活判定：该 (cid, 容器pid) 是否仍在宿主机 /proc 中
			if aliveSet := alive[cid]; aliveSet == nil || !aliveSet[pid] {
				delete(fs.pids, pid)
				continue
			}
			live += st.live
			total += st.total
			freed += st.freed
			npid++
		}
		meta, ok := cidMap[cid]
		if !ok {
			orphans++
			// 孤儿且文件够老 → 删除
			path := filepath.Join(e.dir, cid+".jsonl")
			if fi, err := os.Stat(path); err == nil && time.Since(fi.ModTime()) > gcAfter {
				if err := os.Remove(path); err == nil {
					log.Printf("gc orphan file %s (mtime %s)", cid[:12], fi.ModTime().Format(time.RFC3339))
					delete(e.files, cid)
					continue
				}
			}
		}
		key := cid
		aggBy[key] = &agg{live: live, total: total, freed: freed, pids: npid}
		if ok {
			labelsBy[key] = prometheus.Labels{"node": e.nodeName, "namespace": meta.ns, "pod_name": meta.pod, "container_name": meta.container}
		} else {
			labelsBy[key] = prometheus.Labels{"node": e.nodeName, "namespace": "<orphan>", "pod_name": cid[:12], "container_name": "-"}
		}
	}

	// 3. 重建指标序列（Reset 防陈旧序列残留）
	e.live.Reset()
	e.total.Reset()
	e.freed.Reset()
	e.pids.Reset()
	for key, a := range aggBy {
		l := labelsBy[key]
		e.live.With(l).Set(float64(a.live))
		e.total.With(l).Set(float64(a.total))
		e.freed.With(l).Set(float64(a.freed))
		e.pids.With(l).Set(float64(a.pids))
	}
	e.orphans.Set(float64(orphans))
}

// tailFile 增量读取一个记账文件；文件被截断（size < offset）时重置状态
func (e *exporter) tailFile(cid string) error {
	path := filepath.Join(e.dir, cid+".jsonl")
	st, err := os.Stat(path)
	if err != nil {
		return err
	}
	fs, ok := e.files[cid]
	if !ok {
		fs = &fileState{pids: map[int]*procState{}}
		e.files[cid] = fs
	}
	if st.Size() < fs.offset { // 截断（容器复用文件名等异常）
		fs.offset = 0
		fs.pids = map[int]*procState{}
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := f.Seek(fs.offset, 0); err != nil {
		return err
	}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 4096), 65536)
	var consumed int64
	for sc.Scan() {
		line := sc.Bytes()
		if len(line) == 0 {
			consumed += 1
			continue
		}
		var ev event
		if err := json.Unmarshal(line, &ev); err != nil {
			consumed += int64(len(line)) + 1
			continue // 解析失败的行跳过（可能被截断的尾巴，下轮重读）
		}
		switch ev.T {
		case "S":
			if ev.PID != 0 {
				p := fs.pids[ev.PID]
				if p == nil {
					p = &procState{}
					fs.pids[ev.PID] = p
				}
				if ev.HPID > 0 {
					p.hpid = ev.HPID
				}
				p.live, p.total, p.freed = ev.Live, ev.Total, ev.Freed
			}
		case "X":
			delete(fs.pids, ev.PID)
		}
		consumed += int64(len(line)) + 1
	}
	// 只推进到完整行（Scanner 的 Err 可能是行超长/IO 错）
	if sc.Err() == nil {
		fs.offset += consumed
	}
	return nil
}
