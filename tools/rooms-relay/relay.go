// GeneralsX @feature Android port 04/10/2026 Rooms relay: a virtual LAN over WebSocket.
// Protocol: docs/port/ROOMS_PROTOCOL.md.
package main

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

const (
	protocolVersion = 1
	frameMagic      = 0x47585231 // "GXR1"
	headerSize      = 16
	maxPayload      = 16384
	maxControlBytes = 256 << 10
	ipBase          = 0x0AF00000 // 10.240.0.0
	broadcastIP     = 0xFFFFFFFF
	maxSlots        = 8
	maxQueuedBytes  = 1 << 20
	maxFramesPerSec = 1000
	maxBytesPerSec  = 4 << 20
	resumeWindow    = 60 * time.Second
	writeTimeout    = 10 * time.Second
	pingInterval    = 15 * time.Second
	codeAlphabet    = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
	codeLength      = 6
	maxFilesListed  = 4096
)

// lanPort is true for the game's LAN ports: 8086 is the LAN lobby, 8088 and up the match.
func lanPort(p uint16) bool { return p == 8086 || (p >= 8088 && p <= 8095) }

func slotIP(slot int) uint32 { return ipBase + uint32(slot) }

func randomHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

func randomCode() string {
	b := make([]byte, codeLength)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	for i := range b {
		b[i] = codeAlphabet[int(b[i])%len(codeAlphabet)]
	}
	return string(b)
}

type outFrame struct {
	typ  websocket.MessageType
	data []byte
}

// member is one player: a slot in a room, with or without a live connection.
type member struct {
	hub    *Hub
	name   string
	device string
	game   string
	compat string

	room  *room
	slot  int
	token string
	ready bool
	files map[string]string // nil until the player sends "files"

	// Connection state, replaced on resume. Guarded by the hub's lock.
	conn         *websocket.Conn
	out          chan outFrame
	cancel       context.CancelFunc
	queued       atomic.Int64
	disconnected time.Time

	rateSecond int64
	rateFrames int
	rateBytes  int
}

type room struct {
	code     string
	title    string
	game     string
	compat   string
	capacity int
	public   bool
	password string
	locked   bool
	slots    [maxSlots + 1]*member
}

func (r *room) count() int {
	n := 0
	for _, m := range r.slots[1:] {
		if m != nil {
			n++
		}
	}
	return n
}

// Hub owns every room. One lock: control messages are rare, and the frame path only reads
// the slot table under it to pick the receivers.
type Hub struct {
	mu       sync.Mutex
	name     string
	maxRooms int
	maxConns int
	rooms    map[string]*room
	tokens   map[string]*member
	conns    int
}

func NewHub(name string, maxRooms, maxConns int) *Hub {
	return &Hub{name: name, maxRooms: maxRooms, maxConns: maxConns,
		rooms: map[string]*room{}, tokens: map[string]*member{}}
}

type helloMsg struct {
	Type     string            `json:"type"`
	Protocol int               `json:"protocol"`
	Game     string            `json:"game"`
	Compat   string            `json:"compat"`
	Name     string            `json:"name"`
	Device   string            `json:"device"`
	Capacity int               `json:"capacity"`
	Title    string            `json:"title"`
	Password string            `json:"password"`
	Public   bool              `json:"public"`
	Code     string            `json:"code"`
	Token    string            `json:"token"`
	Ready    bool              `json:"ready"`
	List     map[string]string `json:"list"`
}

type errorReply struct {
	Type  string `json:"type"`
	Code  string `json:"code"`
	Retry bool   `json:"retry"`
}

func clip(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		r = r[:n]
	}
	return string(r)
}

func validDevice(s string) bool {
	if len(s) != 32 {
		return false
	}
	_, err := hex.DecodeString(s)
	return err == nil
}

// Serve runs one connection until it ends.
func (h *Hub) Serve(ctx context.Context, c *websocket.Conn) {
	h.mu.Lock()
	if h.conns >= h.maxConns {
		h.mu.Unlock()
		c.Close(websocket.StatusTryAgainLater, "server_full")
		return
	}
	h.conns++
	h.mu.Unlock()
	defer func() {
		h.mu.Lock()
		h.conns--
		h.mu.Unlock()
	}()

	c.SetReadLimit(maxControlBytes)
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	m, err := h.handshake(ctx, c, cancel)
	if err != nil {
		writeJSON(ctx, c, errorReply{Type: "error", Code: err.Error()})
		c.Close(websocket.StatusPolicyViolation, err.Error())
		return
	}
	out := m.out
	go h.writer(ctx, c, out, m, cancel)
	go keepAlive(ctx, c, cancel)

	for {
		typ, data, err := c.Read(ctx)
		if err != nil {
			break
		}
		if typ == websocket.MessageBinary {
			if !h.forward(m, data) {
				break
			}
			continue
		}
		var msg helloMsg
		if json.Unmarshal(data, &msg) != nil {
			h.sendError(m, "bad_request", false)
			continue
		}
		if msg.Type == "leave" {
			h.leave(m)
			c.Close(websocket.StatusNormalClosure, "left")
			return
		}
		h.control(m, &msg)
	}
	h.dropped(m, c)
	c.CloseNow()
}

func writeJSON(ctx context.Context, c *websocket.Conn, v any) {
	b, _ := json.Marshal(v)
	wctx, cancel := context.WithTimeout(ctx, writeTimeout)
	defer cancel()
	c.Write(wctx, websocket.MessageText, b)
}

func keepAlive(ctx context.Context, c *websocket.Conn, cancel context.CancelFunc) {
	t := time.NewTicker(pingInterval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			pctx, pcancel := context.WithTimeout(ctx, 3*pingInterval)
			err := c.Ping(pctx)
			pcancel()
			if err != nil {
				cancel()
				return
			}
		}
	}
}

func (h *Hub) writer(ctx context.Context, c *websocket.Conn, out chan outFrame, m *member, cancel context.CancelFunc) {
	for {
		select {
		case <-ctx.Done():
			return
		case f, ok := <-out:
			if !ok {
				c.Close(websocket.StatusPolicyViolation, "closed")
				cancel()
				return
			}
			wctx, wcancel := context.WithTimeout(ctx, writeTimeout)
			err := c.Write(wctx, f.typ, f.data)
			wcancel()
			m.queued.Add(-int64(len(f.data)))
			if err != nil {
				cancel()
				return
			}
		}
	}
}

// handshake reads hello and then create/join/resume, and attaches the connection.
func (h *Hub) handshake(ctx context.Context, c *websocket.Conn, cancel context.CancelFunc) (*member, error) {
	hctx, hcancel := context.WithTimeout(ctx, 20*time.Second)
	defer hcancel()
	var hello helloMsg
	if err := readJSON(hctx, c, &hello); err != nil || hello.Type != "hello" {
		return nil, errors.New("protocol")
	}
	if hello.Protocol != protocolVersion {
		return nil, errors.New("protocol")
	}
	if (hello.Game != "zh" && hello.Game != "generals") || hello.Compat == "" || len(hello.Compat) > 200 || !validDevice(hello.Device) {
		return nil, errors.New("bad_request")
	}
	var req helloMsg
	if err := readJSON(hctx, c, &req); err != nil {
		return nil, errors.New("protocol")
	}

	h.mu.Lock()
	defer h.mu.Unlock()
	m := &member{hub: h, name: clip(hello.Name, 24), device: hello.Device, game: hello.Game, compat: hello.Compat}
	if m.name == "" {
		m.name = "Player"
	}
	switch req.Type {
	case "create":
		if len(h.rooms) >= h.maxRooms {
			return nil, errors.New("server_full")
		}
		if req.Capacity < 2 || req.Capacity > maxSlots {
			return nil, errors.New("bad_request")
		}
		r := &room{title: clip(req.Title, 40), game: hello.Game, compat: hello.Compat,
			capacity: req.Capacity, public: req.Public, password: clip(req.Password, 64)}
		for {
			r.code = randomCode()
			if h.rooms[r.code] == nil {
				break
			}
		}
		h.rooms[r.code] = r
		h.seat(r, 1, m)
	case "join":
		r := h.rooms[req.Code]
		if r == nil {
			return nil, errors.New("no_room")
		}
		if r.game != hello.Game {
			return nil, errors.New("game_mismatch")
		}
		if r.compat != hello.Compat {
			return nil, errors.New("compat_mismatch")
		}
		if r.password != "" && subtle.ConstantTimeCompare([]byte(r.password), []byte(req.Password)) != 1 {
			return nil, errors.New("password")
		}
		if r.locked {
			return nil, errors.New("started")
		}
		slot := 0
		for s := 1; s <= r.capacity; s++ {
			if r.slots[s] == nil {
				slot = s
				break
			}
		}
		if slot == 0 {
			return nil, errors.New("full")
		}
		h.seat(r, slot, m)
	case "resume":
		old := h.tokens[req.Token]
		if old == nil || old.device != hello.Device || old.room == nil {
			return nil, errors.New("resume_expired")
		}
		// A phone that switched networks reconnects before the old connection has timed out:
		// the new one takes over.
		if old.conn != nil {
			old.cancel()
			h.cut(old)
		}
		m = old
	default:
		return nil, errors.New("protocol")
	}
	m.conn = c
	m.out = make(chan outFrame, 4096)
	m.cancel = cancel
	m.queued.Store(0)
	m.disconnected = time.Time{}
	h.broadcastRoom(m.room)
	return m, nil
}

func readJSON(ctx context.Context, c *websocket.Conn, v any) error {
	typ, data, err := c.Read(ctx)
	if err != nil {
		return err
	}
	if typ != websocket.MessageText {
		return errors.New("binary")
	}
	return json.Unmarshal(data, v)
}

// seat puts m into slot of r. Caller holds h.mu.
func (h *Hub) seat(r *room, slot int, m *member) {
	m.room = r
	m.slot = slot
	m.token = randomHex(16)
	r.slots[slot] = m
	h.tokens[m.token] = m
}

// enqueue queues a frame for m without blocking. A receiver that falls 1 MB behind is cut off:
// a lockstep game cannot use late packets, and waiting for it would stall every other player.
// Caller holds h.mu.
func (h *Hub) enqueue(m *member, f outFrame) {
	if m.out == nil {
		return
	}
	if m.queued.Load()+int64(len(f.data)) > maxQueuedBytes {
		h.cut(m)
		return
	}
	select {
	case m.out <- f:
		m.queued.Add(int64(len(f.data)))
	default:
		h.cut(m)
	}
}

// cut ends m's connection; the slot stays resumable. Caller holds h.mu.
func (h *Hub) cut(m *member) {
	if m.out != nil {
		close(m.out)
		m.out = nil
	}
}

func (h *Hub) sendJSON(m *member, v any) {
	b, _ := json.Marshal(v)
	h.enqueue(m, outFrame{typ: websocket.MessageText, data: b})
}

func (h *Hub) sendError(m *member, code string, retry bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.sendJSON(m, errorReply{Type: "error", Code: code, Retry: retry})
}

type memberView struct {
	Slot      int    `json:"slot"`
	Name      string `json:"name"`
	Ready     bool   `json:"ready"`
	Connected bool   `json:"connected"`
	Files     string `json:"files"`
}

type roomView struct {
	Type      string       `json:"type"`
	Code      string       `json:"code"`
	Slot      int          `json:"slot"`
	IP        uint32       `json:"ip"`
	Capacity  int          `json:"capacity"`
	Title     string       `json:"title"`
	Game      string       `json:"game"`
	Public    bool         `json:"public"`
	Password  bool         `json:"password"`
	Locked    bool         `json:"locked"`
	CanLaunch bool         `json:"canLaunch"`
	AllReady  bool         `json:"allReady"`
	Token     string       `json:"token"`
	Members   []memberView `json:"members"`
}

// filesState compares m's list with the creator's. Caller holds h.mu.
func filesState(r *room, m *member) (state string, differs, missing, extra []string) {
	ref := r.slots[1]
	if m.files == nil || ref == nil || ref.files == nil {
		return "pending", nil, nil, nil
	}
	if m == ref {
		return "ok", nil, nil, nil
	}
	for p, h := range ref.files {
		mh, ok := m.files[p]
		if !ok {
			missing = append(missing, p)
		} else if mh != h {
			differs = append(differs, p)
		}
	}
	for p := range m.files {
		if _, ok := ref.files[p]; !ok {
			extra = append(extra, p)
		}
	}
	sort.Strings(differs)
	sort.Strings(missing)
	sort.Strings(extra)
	if len(differs)+len(missing)+len(extra) == 0 {
		return "ok", nil, nil, nil
	}
	return "differs", differs, missing, extra
}

// readiness reports whether everybody is ready, and whether the creator may launch.
// Caller holds h.mu.
func readiness(r *room) (allReady, canLaunch bool) {
	n := 0
	allReady, filesOK := true, true
	for _, m := range r.slots[1:] {
		if m == nil {
			continue
		}
		n++
		if !m.ready || m.conn == nil {
			allReady = false
		}
		if st, _, _, _ := filesState(r, m); st != "ok" {
			filesOK = false
		}
	}
	allReady = allReady && n >= 2
	return allReady, allReady && filesOK && !r.locked
}

// broadcastRoom sends every member its view of r. Caller holds h.mu.
func (h *Hub) broadcastRoom(r *room) {
	if r == nil {
		return
	}
	allReady, canLaunch := readiness(r)
	views := make([]memberView, 0, maxSlots)
	for _, m := range r.slots[1:] {
		if m == nil {
			continue
		}
		st, _, _, _ := filesState(r, m)
		views = append(views, memberView{Slot: m.slot, Name: m.name, Ready: m.ready, Connected: m.conn != nil, Files: st})
	}
	for _, m := range r.slots[1:] {
		if m == nil || m.out == nil {
			continue
		}
		h.sendJSON(m, roomView{Type: "room", Code: r.code, Slot: m.slot, IP: slotIP(m.slot),
			Capacity: r.capacity, Title: r.title, Game: r.game, Public: r.public, Password: r.password != "",
			Locked: r.locked, CanLaunch: canLaunch, AllReady: allReady, Token: m.token, Members: views})
	}
}

type filesReply struct {
	Type    string   `json:"type"`
	Differs []string `json:"differs"`
	Missing []string `json:"missing"`
	Extra   []string `json:"extra"`
}

func (h *Hub) control(m *member, msg *helloMsg) {
	h.mu.Lock()
	defer h.mu.Unlock()
	r := m.room
	if r == nil {
		return
	}
	switch msg.Type {
	case "ready":
		if r.locked {
			return
		}
		m.ready = msg.Ready
	case "files":
		if len(msg.List) > maxFilesListed {
			h.sendJSON(m, errorReply{Type: "error", Code: "bad_request"})
			return
		}
		m.files = msg.List
		// The creator's list is the reference, so a new one re-judges everybody.
		targets := []*member{m}
		if m.slot == 1 {
			targets = r.slots[2:]
		}
		for _, t := range targets {
			if t == nil {
				continue
			}
			if st, d, mi, e := filesState(r, t); st == "differs" {
				h.sendJSON(t, filesReply{Type: "files", Differs: d, Missing: mi, Extra: e})
			}
		}
	case "capacity":
		if m.slot != 1 || r.locked || msg.Capacity < 2 || msg.Capacity > maxSlots {
			h.sendJSON(m, errorReply{Type: "error", Code: "not_creator"})
			return
		}
		for s := msg.Capacity + 1; s <= maxSlots; s++ {
			if r.slots[s] != nil {
				h.sendJSON(m, errorReply{Type: "error", Code: "bad_request"})
				return
			}
		}
		r.capacity = msg.Capacity
	case "launch":
		if m.slot != 1 {
			h.sendJSON(m, errorReply{Type: "error", Code: "not_creator"})
			return
		}
		if _, can := readiness(r); !can {
			h.sendJSON(m, errorReply{Type: "error", Code: "not_ready"})
			return
		}
		r.locked = true
		for _, t := range r.slots[1:] {
			if t != nil {
				h.sendJSON(t, map[string]string{"type": "launch"})
			}
		}
	default:
		h.sendJSON(m, errorReply{Type: "error", Code: "bad_request"})
		return
	}
	h.broadcastRoom(r)
}

// forward relays one game datagram. It returns false when the sender must be disconnected.
func (h *Hub) forward(m *member, data []byte) bool {
	if len(data) < headerSize || len(data) > headerSize+maxPayload {
		return false
	}
	if binary.BigEndian.Uint32(data[0:4]) != frameMagic {
		return false
	}
	src := binary.BigEndian.Uint32(data[4:8])
	dst := binary.BigEndian.Uint32(data[8:12])
	sport := binary.BigEndian.Uint16(data[12:14])
	dport := binary.BigEndian.Uint16(data[14:16])

	h.mu.Lock()
	defer h.mu.Unlock()
	r := m.room
	if r == nil || src != slotIP(m.slot) || !lanPort(sport) || !lanPort(dport) {
		return false
	}
	now := time.Now().Unix()
	if now != m.rateSecond {
		m.rateSecond, m.rateFrames, m.rateBytes = now, 0, 0
	}
	m.rateFrames++
	m.rateBytes += len(data)
	if m.rateFrames > maxFramesPerSec || m.rateBytes > maxBytesPerSec {
		return false
	}
	// The frame is passed on as received; receivers check the header themselves.
	f := outFrame{typ: websocket.MessageBinary, data: data}
	if dst == broadcastIP {
		for _, t := range r.slots[1:] {
			if t != nil && t != m {
				h.enqueue(t, f)
			}
		}
		return true
	}
	if dst <= ipBase || dst > ipBase+maxSlots {
		return false
	}
	if t := r.slots[dst-ipBase]; t != nil && t != m {
		h.enqueue(t, f)
	}
	return true
}

// leave removes m for good. The creator leaving before launch closes the room for everybody.
func (h *Hub) leave(m *member) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.remove(m)
}

// remove takes m out of its room. Caller holds h.mu.
func (h *Hub) remove(m *member) {
	r := m.room
	if r == nil {
		return
	}
	delete(h.tokens, m.token)
	r.slots[m.slot] = nil
	m.room = nil
	if m.slot == 1 && !r.locked {
		for _, t := range r.slots[1:] {
			if t != nil {
				h.sendJSON(t, errorReply{Type: "error", Code: "closed"})
				delete(h.tokens, t.token)
				t.room = nil
				h.cut(t)
			}
		}
		delete(h.rooms, r.code)
		return
	}
	if r.count() == 0 {
		delete(h.rooms, r.code)
		return
	}
	h.broadcastRoom(r)
}

// dropped marks m disconnected; its slot can be resumed for resumeWindow.
func (h *Hub) dropped(m *member, c *websocket.Conn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if m.conn != c {
		return // already resumed on another connection
	}
	h.cut(m)
	m.conn = nil
	m.cancel = nil
	m.disconnected = time.Now()
	if m.room != nil {
		h.broadcastRoom(m.room)
	}
}

// Sweep removes members whose resume window has passed. Run it periodically.
func (h *Hub) Sweep(now time.Time) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, r := range h.rooms {
		for _, m := range r.slots[1:] {
			if m != nil && m.conn == nil && !m.disconnected.IsZero() && now.Sub(m.disconnected) > resumeWindow {
				h.remove(m)
			}
		}
	}
}

type healthView struct {
	Protocol int    `json:"protocol"`
	Version  string `json:"version"`
	Name     string `json:"name"`
	Rooms    int    `json:"rooms"`
	Players  int    `json:"players"`
	MaxRooms int    `json:"maxRooms"`
}

func (h *Hub) Health() healthView {
	h.mu.Lock()
	defer h.mu.Unlock()
	players := 0
	for _, r := range h.rooms {
		players += r.count()
	}
	return healthView{Protocol: protocolVersion, Version: version, Name: h.name, Rooms: len(h.rooms), Players: players, MaxRooms: h.maxRooms}
}

type publicRoom struct {
	Code     string `json:"code"`
	Title    string `json:"title"`
	Game     string `json:"game"`
	Compat   string `json:"compat"`
	Members  int    `json:"members"`
	Capacity int    `json:"capacity"`
	Password bool   `json:"password"`
}

func (h *Hub) PublicRooms() []publicRoom {
	h.mu.Lock()
	defer h.mu.Unlock()
	list := []publicRoom{}
	for _, r := range h.rooms {
		n := r.count()
		if !r.public || r.locked || n >= r.capacity {
			continue
		}
		list = append(list, publicRoom{Code: r.code, Title: r.title, Game: r.game, Compat: r.compat,
			Members: n, Capacity: r.capacity, Password: r.password != ""})
	}
	sort.Slice(list, func(i, j int) bool { return list[i].Code < list[j].Code })
	return list
}
