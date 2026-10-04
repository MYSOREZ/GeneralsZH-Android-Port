// GeneralsX @feature Android port 04/10/2026 Rooms relay server. Run it behind a TLS proxy
// (docker-compose.yml puts Caddy in front); see docs/HOWTO/ROOMS_SERVER.md.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/coder/websocket"
)

var version = "dev"

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func envInt(key string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(key)); err == nil && v > 0 {
		return v
	}
	return def
}

func main() {
	listen := flag.String("listen", envOr("RELAY_LISTEN", ":8080"), "address to listen on")
	name := flag.String("name", envOr("RELAY_NAME", "Rooms server"), "name shown in the launcher")
	maxRooms := flag.Int("max-rooms", envInt("RELAY_MAX_ROOMS", 200), "maximum rooms")
	maxConns := flag.Int("max-players", envInt("RELAY_MAX_PLAYERS", 1000), "maximum connections")
	flag.Parse()

	hub := NewHub(*name, *maxRooms, *maxConns)
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		c, err := websocket.Accept(w, r, nil)
		if err != nil {
			return
		}
		hub.Serve(r.Context(), c)
	})
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSONHTTP(w, hub.Health())
	})
	mux.HandleFunc("/rooms", func(w http.ResponseWriter, r *http.Request) {
		writeJSONHTTP(w, map[string]any{"rooms": hub.PublicRooms()})
	})

	srv := &http.Server{Addr: *listen, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go func() {
		t := time.NewTicker(5 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case now := <-t.C:
				hub.Sweep(now)
			}
		}
	}()
	go func() {
		<-ctx.Done()
		sctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		srv.Shutdown(sctx)
	}()
	log.Printf("rooms relay %s listening on %s (%q, %d rooms, %d players)", version, *listen, *name, *maxRooms, *maxConns)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}

func writeJSONHTTP(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	json.NewEncoder(w).Encode(v)
}
