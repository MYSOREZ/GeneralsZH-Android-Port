package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

type client struct {
	t *testing.T
	c *websocket.Conn
}

func newServer(t *testing.T) (*Hub, string) {
	hub := NewHub("test", 10, 50)
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		c, err := websocket.Accept(w, r, nil)
		if err != nil {
			return
		}
		hub.Serve(r.Context(), c)
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return hub, "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws"
}

func dial(t *testing.T, url, device string, first map[string]any, compat string) *client {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	c, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	c.SetReadLimit(maxControlBytes)
	cl := &client{t: t, c: c}
	cl.send(map[string]any{"type": "hello", "protocol": 1, "game": "zh", "compat": compat, "name": "p", "device": device})
	cl.send(first)
	return cl
}

func (cl *client) send(v any) {
	b, _ := json.Marshal(v)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := cl.c.Write(ctx, websocket.MessageText, b); err != nil {
		cl.t.Fatal(err)
	}
}

func (cl *client) sendFrame(src, dst uint32, sport, dport uint16, payload string) {
	b := make([]byte, headerSize+len(payload))
	binary.BigEndian.PutUint32(b[0:], frameMagic)
	binary.BigEndian.PutUint32(b[4:], src)
	binary.BigEndian.PutUint32(b[8:], dst)
	binary.BigEndian.PutUint16(b[12:], sport)
	binary.BigEndian.PutUint16(b[14:], dport)
	copy(b[16:], payload)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := cl.c.Write(ctx, websocket.MessageBinary, b); err != nil {
		cl.t.Fatal(err)
	}
}

// next returns the next text message of the given type, skipping others; binary frames are
// returned as {"type":"frame","payload":...}.
func (cl *client) next(want string) map[string]any {
	cl.t.Helper()
	for {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		typ, data, err := cl.c.Read(ctx)
		cancel()
		if err != nil {
			cl.t.Fatalf("waiting for %q: %v", want, err)
		}
		var m map[string]any
		if typ == websocket.MessageBinary {
			m = map[string]any{"type": "frame", "payload": string(data[headerSize:]),
				"src": float64(binary.BigEndian.Uint32(data[4:]))}
		} else if err := json.Unmarshal(data, &m); err != nil {
			cl.t.Fatal(err)
		}
		if m["type"] == want {
			return m
		}
	}
}

const devA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
const devB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
const devC = "cccccccccccccccccccccccccccccccc"

func TestRoomLifecycle(t *testing.T) {
	hub, url := newServer(t)
	a := dial(t, url, devA, map[string]any{"type": "create", "capacity": 2, "title": "t", "password": "pw", "public": true}, "zh-3295-30")
	ra := a.next("room")
	code := ra["code"].(string)
	if ra["slot"].(float64) != 1 || uint32(ra["ip"].(float64)) != slotIP(1) {
		t.Fatalf("creator view %v", ra)
	}
	if got := hub.PublicRooms(); len(got) != 1 || !got[0].Password {
		t.Fatalf("public rooms %v", got)
	}

	bad := dial(t, url, devC, map[string]any{"type": "join", "code": code, "password": "no"}, "zh-3295-30")
	if e := bad.next("error"); e["code"] != "password" {
		t.Fatalf("wrong password: %v", e)
	}
	other := dial(t, url, devC, map[string]any{"type": "join", "code": code, "password": "pw"}, "zh-3295-60")
	if e := other.next("error"); e["code"] != "compat_mismatch" {
		t.Fatalf("compat: %v", e)
	}

	b := dial(t, url, devB, map[string]any{"type": "join", "code": code, "password": "pw"}, "zh-3295-30")
	rb := b.next("room")
	if rb["slot"].(float64) != 2 {
		t.Fatalf("joiner view %v", rb)
	}

	a.send(map[string]any{"type": "files", "list": map[string]string{"INIZH.big": "1", "MapsZH.big": "2"}})
	b.send(map[string]any{"type": "files", "list": map[string]string{"INIZH.big": "1", "MapsZH.big": "x"}})
	if f := b.next("files"); len(f["differs"].([]any)) != 1 {
		t.Fatalf("files diff %v", f)
	}
	a.send(map[string]any{"type": "ready", "ready": true})
	b.send(map[string]any{"type": "ready", "ready": true})
	a.send(map[string]any{"type": "launch"})
	if e := a.next("error"); e["code"] != "not_ready" {
		t.Fatalf("launch with differing files: %v", e)
	}
	b.send(map[string]any{"type": "files", "list": map[string]string{"INIZH.big": "1", "MapsZH.big": "2"}})
	for {
		if r := a.next("room"); r["canLaunch"] == true {
			break
		}
	}
	a.send(map[string]any{"type": "launch"})
	a.next("launch")
	b.next("launch")

	// Unicast, broadcast, and a spoofed source that must cost the sender its connection.
	a.sendFrame(slotIP(1), slotIP(2), 8088, 8088, "hello")
	if f := b.next("frame"); f["payload"] != "hello" || uint32(f["src"].(float64)) != slotIP(1) {
		t.Fatalf("unicast %v", f)
	}
	b.sendFrame(slotIP(2), broadcastIP, 8086, 8086, "lobby")
	if f := a.next("frame"); f["payload"] != "lobby" {
		t.Fatalf("broadcast %v", f)
	}
	b.sendFrame(slotIP(1), slotIP(1), 8088, 8088, "spoof")
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	for {
		if _, _, err := b.c.Read(ctx); err != nil {
			break
		}
	}

	// The slot stays resumable; the joiner comes back with its token.
	token := rb["token"].(string)
	b2 := dial(t, url, devB, map[string]any{"type": "resume", "token": token}, "zh-3295-30")
	if r := b2.next("room"); r["slot"].(float64) != 2 || r["locked"] != true {
		t.Fatalf("resume %v", r)
	}
	a.sendFrame(slotIP(1), slotIP(2), 8088, 8088, "again")
	if f := b2.next("frame"); f["payload"] != "again" {
		t.Fatalf("after resume %v", f)
	}
}

func TestCreatorLeavingClosesRoom(t *testing.T) {
	hub, url := newServer(t)
	a := dial(t, url, devA, map[string]any{"type": "create", "capacity": 4}, "c")
	code := a.next("room")["code"].(string)
	b := dial(t, url, devB, map[string]any{"type": "join", "code": code}, "c")
	b.next("room")
	a.send(map[string]any{"type": "leave"})
	if e := b.next("error"); e["code"] != "closed" {
		t.Fatalf("closed: %v", e)
	}
	time.Sleep(100 * time.Millisecond)
	if h := hub.Health(); h.Rooms != 0 {
		t.Fatalf("rooms left: %d", h.Rooms)
	}
}

func TestSweepDropsExpiredSlot(t *testing.T) {
	hub, url := newServer(t)
	a := dial(t, url, devA, map[string]any{"type": "create", "capacity": 2}, "c")
	code := a.next("room")["code"].(string)
	b := dial(t, url, devB, map[string]any{"type": "join", "code": code}, "c")
	b.next("room")
	b.c.CloseNow()
	for {
		r := a.next("room")
		if ms := r["members"].([]any); len(ms) == 2 && ms[1].(map[string]any)["connected"] == false {
			break
		}
	}
	hub.Sweep(time.Now().Add(2 * resumeWindow))
	if r := a.next("room"); len(r["members"].([]any)) != 1 {
		t.Fatalf("expired slot kept: %v", r)
	}
}
