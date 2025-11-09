package main

import (
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"sync"
)

var (
	port = flag.Int("port", 8080, "The server port")
)

// PageServer implements the HTTP Page Server
type PageServer struct {
	// In-memory page cache (in production, this would be backed by object storage)
	pages map[string][]byte
	mu    sync.RWMutex
	
	// WAL storage (in production, this would be persistent)
	walRecords []WALRecord
	walMu      sync.RWMutex
}

// Request/Response structures
type GetPageRequest struct {
	SpaceID uint32 `json:"space_id"`
	PageNo  uint32 `json:"page_no"`
	LSN     uint64 `json:"lsn"`
}

type GetPageResponse struct {
	Status   string `json:"status"`
	PageData string `json:"page_data,omitempty"` // Base64 encoded
	PageLSN  uint64 `json:"page_lsn,omitempty"`
	Error    string `json:"error,omitempty"`
}

type StreamWALRequest struct {
	LSN     uint64 `json:"lsn"`
	WALData string `json:"wal_data"` // Base64 encoded
	SpaceID uint32 `json:"space_id,omitempty"`
	PageNo  uint32 `json:"page_no,omitempty"`
}

type StreamWALResponse struct {
	Status         string `json:"status"`
	LastAppliedLSN uint64 `json:"last_applied_lsn,omitempty"`
	Error          string `json:"error,omitempty"`
}

type WALRecord struct {
	LSN     uint64
	WALData []byte
	SpaceID uint32
	PageNo  uint32
}

type PingResponse struct {
	Status  string `json:"status"`
	Version string `json:"version"`
}

func NewPageServer() *PageServer {
	return &PageServer{
		pages:      make(map[string][]byte),
		walRecords: make([]WALRecord, 0),
	}
}

// HTTP Handlers

func (s *PageServer) handleGetPage(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req GetPageRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	key := fmt.Sprintf("%d:%d", req.SpaceID, req.PageNo)
	
	s.mu.RLock()
	pageData, exists := s.pages[key]
	s.mu.RUnlock()
	
	if !exists {
		resp := GetPageResponse{
			Status: "error",
			Error:  fmt.Sprintf("Page not found: space=%d page=%d", req.SpaceID, req.PageNo),
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(resp)
		return
	}
	
	// Base64 encode page data
	pageDataB64 := base64.StdEncoding.EncodeToString(pageData)
	
	resp := GetPageResponse{
		Status:   "success",
		PageData: pageDataB64,
		PageLSN:  req.LSN, // In production, return actual page LSN
	}
	
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func (s *PageServer) handleStreamWAL(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req StreamWALRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	// Decode base64 WAL data
	walData, err := base64.StdEncoding.DecodeString(req.WALData)
	if err != nil {
		http.Error(w, "Invalid base64 WAL data", http.StatusBadRequest)
		return
	}

	// Store WAL record
	record := WALRecord{
		LSN:     req.LSN,
		WALData: walData,
		SpaceID: req.SpaceID,
		PageNo:  req.PageNo,
	}

	s.walMu.Lock()
	s.walRecords = append(s.walRecords, record)
	s.walMu.Unlock()

	// In production, we would:
	// 1. Apply WAL record to affected pages
	// 2. Update page versions with new LSN
	// 3. Store pages to object storage

	log.Printf("Received WAL record: LSN=%d space=%d page=%d len=%d",
		req.LSN, req.SpaceID, req.PageNo, len(walData))

	resp := StreamWALResponse{
		Status:         "success",
		LastAppliedLSN: req.LSN,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func (s *PageServer) handlePing(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	resp := PingResponse{
		Status:  "ok",
		Version: "1.0.0",
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func main() {
	flag.Parse()
	
	pageServer := NewPageServer()
	
	// Register HTTP handlers
	http.HandleFunc("/api/v1/get_page", pageServer.handleGetPage)
	http.HandleFunc("/api/v1/stream_wal", pageServer.handleStreamWAL)
	http.HandleFunc("/api/v1/ping", pageServer.handlePing)
	
	log.Printf("Page Server listening on port %d", *port)
	log.Printf("Endpoints:")
	log.Printf("  POST /api/v1/get_page")
	log.Printf("  POST /api/v1/stream_wal")
	log.Printf("  GET  /api/v1/ping")
	
	if err := http.ListenAndServe(fmt.Sprintf(":%d", *port), nil); err != nil {
		log.Fatalf("failed to serve: %v", err)
	}
}

