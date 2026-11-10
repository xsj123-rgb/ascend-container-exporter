// ascend-mutating-webhook — 为请求昇腾 NPU 资源的 Pod 自动注入显存记账探针
//
// 与 HAMi 的 vgpu.hami.io webhook 共存设计：
//   - 只补 env(LD_PRELOAD) / volumeMounts / volumes 三处，字段与 HAMi 的
//     annotation/调度链完全不相交；绝不触碰 nodeName/schedulerName/resources
//   - 幂等：已含 npu-account-* 卷或 LD_PRELOAD 已带 shim 路径的 Pod 直接放行
//   - failurePolicy 建议配 Ignore：本服务不可用只影响监控，不阻塞业务创建
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"strings"

	admissionv1 "k8s.io/api/admission/v1"
	corev1 "k8s.io/api/core/v1"
)

const (
	shimPath   = "/opt/npu-account/libnpu-account.so"
	libVol     = "npu-account-lib"
	dataVol    = "npu-account-data"
	libMount   = "/opt/npu-account"
	dataMount  = "/run/npu-account"
	envName    = "LD_PRELOAD"
	rcName     = "ascend" // 集群的 RuntimeClass 名
	ascendPre  = "huawei.com/Ascend"
)

var (
	addRuntimeClass bool // Pod 未写 runtimeClassName 时是否自动补
)

type patchOp struct {
	Op    string      `json:"op"`
	Path  string      `json:"path"`
	Value interface{} `json:"value,omitempty"`
}

func usesAscend(c corev1.Container) bool {
	for k := range c.Resources.Requests {
		if strings.HasPrefix(string(k), ascendPre) {
			return true
		}
	}
	return false
}

func hasInjected(pod *corev1.Pod) bool {
	for _, v := range pod.Spec.Volumes {
		if v.Name == libVol {
			return true
		}
	}
	return false
}

// buildPatch 生成 RFC6902 JSON Patch；显式处理 env/volumeMounts/volumes 键缺失的情况
func buildPatch(pod *corev1.Pod) []patchOp {
	var ops []patchOp

	patchContainer := func(field string, idx int, c corev1.Container) {
		base := fmt.Sprintf("/spec/%s/%d", field, idx)
		// ① env: LD_PRELOAD
		envIdx := -1
		for i, e := range c.Env {
			if e.Name == envName {
				envIdx = i
				break
			}
		}
		val := shimPath
		switch {
		case envIdx >= 0 && strings.Contains(c.Env[envIdx].Value, shimPath):
			// 已注入
		case envIdx >= 0: // 已有别的 LD_PRELOAD（如 jemalloc）→ 冒号追加
			val = c.Env[envIdx].Value + ":" + shimPath
			ops = append(ops, patchOp{"replace", fmt.Sprintf("%s/env/%d", base, envIdx),
				corev1.EnvVar{Name: envName, Value: val}})
		case len(c.Env) > 0:
			ops = append(ops, patchOp{"add", base + "/env/-",
				corev1.EnvVar{Name: envName, Value: val}})
		default:
			ops = append(ops, patchOp{"add", base + "/env",
				[]corev1.EnvVar{{Name: envName, Value: val}}})
		}
		// ② volumeMounts
		if len(c.VolumeMounts) > 0 {
			ops = append(ops, patchOp{"add", base + "/volumeMounts/-",
				corev1.VolumeMount{Name: libVol, MountPath: libMount, ReadOnly: true}})
			ops = append(ops, patchOp{"add", base + "/volumeMounts/-",
				corev1.VolumeMount{Name: dataVol, MountPath: dataMount}})
		} else {
			ops = append(ops, patchOp{"add", base + "/volumeMounts",
				[]corev1.VolumeMount{
					{Name: libVol, MountPath: libMount, ReadOnly: true},
					{Name: dataVol, MountPath: dataMount},
				}})
		}
	}

	for i, c := range pod.Spec.InitContainers {
		if usesAscend(c) {
			patchContainer("initContainers", i, c)
		}
	}
	for i, c := range pod.Spec.Containers {
		if usesAscend(c) {
			patchContainer("containers", i, c)
		}
	}
	// ③ volumes（Pod 级，先挂载声明可能已存在则幂等检查在 hasInjected 里做）
	if len(pod.Spec.Volumes) > 0 {
		ops = append(ops, patchOp{"add", "/spec/volumes/-",
			corev1.Volume{Name: libVol, VolumeSource: corev1.VolumeSource{
				HostPath: &corev1.HostPathVolumeSource{Path: libMount, Type: hostPathTypePtr(corev1.HostPathDirectory)}}}})
		ops = append(ops, patchOp{"add", "/spec/volumes/-",
			corev1.Volume{Name: dataVol, VolumeSource: corev1.VolumeSource{
				HostPath: &corev1.HostPathVolumeSource{Path: dataMount, Type: hostPathTypePtr(corev1.HostPathDirectoryOrCreate)}}}})
	} else {
		ops = append(ops, patchOp{"add", "/spec/volumes",
			[]corev1.Volume{
				{Name: libVol, VolumeSource: corev1.VolumeSource{
					HostPath: &corev1.HostPathVolumeSource{Path: libMount, Type: hostPathTypePtr(corev1.HostPathDirectory)}}},
				{Name: dataVol, VolumeSource: corev1.VolumeSource{
					HostPath: &corev1.HostPathVolumeSource{Path: dataMount, Type: hostPathTypePtr(corev1.HostPathDirectoryOrCreate)}}},
			}})
	}
	// ④ runtimeClassName（可配置；仅当 Pod 未指定时补）
	if addRuntimeClass && pod.Spec.RuntimeClassName == nil {
		rc := rcName
		ops = append(ops, patchOp{"add", "/spec/runtimeClassName", &rc})
	}
	return ops
}

func hostPathTypePtr(t corev1.HostPathType) *corev1.HostPathType { return &t }

func handleMutate(w http.ResponseWriter, r *http.Request) {
	var ar admissionv1.AdmissionReview
	if err := json.NewDecoder(r.Body).Decode(&ar); err != nil || ar.Request == nil {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	resp := admissionv1.AdmissionReview{
		TypeMeta: ar.TypeMeta,
		Response: &admissionv1.AdmissionResponse{UID: ar.Request.UID, Allowed: true},
	}
	// 只处理 Pod 创建（本服务自身/其它非 Ascend Pod 会被 usesAscend 过滤掉）
	if ar.Request.Kind.Kind == "Pod" && ar.Request.Operation == admissionv1.Create {
		var pod corev1.Pod
		if err := json.Unmarshal(ar.Request.Object.Raw, &pod); err == nil &&
			!hasInjected(&pod) && podRequestsAscend(&pod) {
			if ops := buildPatch(&pod); len(ops) > 0 {
				raw, _ := json.Marshal(ops)
				resp.Response.Patch = raw
				pt := admissionv1.PatchTypeJSONPatch
				resp.Response.PatchType = &pt
			}
		}
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(resp)
}

func podRequestsAscend(pod *corev1.Pod) bool {
	for _, c := range append(pod.Spec.InitContainers, pod.Spec.Containers...) {
		if usesAscend(c) {
			return true
		}
	}
	return false
}

func healthz(w http.ResponseWriter, _ *http.Request) {
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ok"))
}

func main() {
	flag.BoolVar(&addRuntimeClass, "add-runtime-class", true,
		"Pod 未写 runtimeClassName 时自动补 ascend（HAMi vNPU 场景必需）")
	flag.Parse()
	http.HandleFunc("/mutate", handleMutate)
	http.HandleFunc("/healthz", healthz)
	log.Println("ascend-mutating-webhook listening :8443")
	log.Fatal(http.ListenAndServeTLS(":8443",
		"/etc/webhook/certs/tls.crt", "/etc/webhook/certs/tls.key", nil))
}
