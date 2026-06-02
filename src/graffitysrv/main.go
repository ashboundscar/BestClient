package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	defaultBindAddress = "0.0.0.0:8781"
	defaultStateFile   = "graffity_state.json"
	maxPerIP           = 3
	graffitySize       = 32
	maxLineBytes       = 4 * 1024
	maxServerAddrLen   = 128
	maxOwnerIDLen      = 64
	maxGraffityIDLen   = 64
	maxEntityIDLen     = 64
	maxActiveConnPerIP = 6
	maxAbsCoordinate   = 1 << 20

	helloTimeout = 8 * time.Second
	idleTimeout  = 45 * time.Second
	writeTimeout = 5 * time.Second
	ipStateTTL   = 10 * time.Minute

	connectRatePerSecond = 2.0
	connectBurst         = 8.0
	messageRatePerSecond = 20.0
	messageBurst         = 40.0
	placeRatePerSecond   = 0.75
	placeBurst           = 3.0
	removeRatePerSecond  = 2.0
	removeBurst          = 6.0
)

var allowedGraffityIDs = map[string]struct{}{
	"best_client_graffity":     {},
	"ego_graffity":             {},
	"cats_with_drool_graffity": {},
}

type inboundMessage struct {
	Type          string `json:"type"`
	ServerAddress string `json:"server_address"`
	OwnerID       string `json:"owner_id"`
	GraffityID    string `json:"graffity_id"`
	ID            string `json:"id"`
	X             int    `json:"x"`
	Y             int    `json:"y"`
	Size          int    `json:"size"`
	Air           bool   `json:"air"`
}

type outboundGraffity struct {
	ID         string `json:"id"`
	GraffityID string `json:"graffity_id"`
	X          int    `json:"x"`
	Y          int    `json:"y"`
	Size       int    `json:"size"`
	Air        bool   `json:"air"`
	Owned      bool   `json:"owned"`
}

type outboundMessage struct {
	Type       string             `json:"type"`
	Message    string             `json:"message,omitempty"`
	Graffities []outboundGraffity `json:"graffities,omitempty"`
}

type persistedState struct {
	UpdatedAt  time.Time           `json:"updated_at"`
	Graffities []persistedGraffity `json:"graffities"`
}

type persistedGraffity struct {
	ID            string `json:"id"`
	GraffityID    string `json:"graffity_id"`
	ServerAddress string `json:"server_address"`
	OwnerID       string `json:"owner_id"`
	X             int    `json:"x"`
	Y             int    `json:"y"`
	Size          int    `json:"size"`
	Air           bool   `json:"air"`
}

type graffity struct {
	ID            string
	GraffityID    string
	ServerAddress string
	OwnerID       string
	X             int
	Y             int
	Size          int
	Air           bool
	Owner         *clientConn
}

type clientConn struct {
	conn          net.Conn
	ownerID       string
	remoteIP      string
	serverAddress string
	mu            sync.Mutex
}

func (c *clientConn) send(v any) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	_ = c.conn.SetWriteDeadline(time.Now().Add(writeTimeout))
	return json.NewEncoder(c.conn).Encode(v)
}

type tokenBucket struct {
	Tokens float64
	Last   time.Time
}

type ipState struct {
	ActiveConns    int
	LastSeen       time.Time
	ConnectLimiter tokenBucket
	MessageLimiter tokenBucket
	PlaceLimiter   tokenBucket
	RemoveLimiter  tokenBucket
}

type serverState struct {
	mu         sync.Mutex
	nextID     uint64
	stateFile  string
	clients    map[*clientConn]struct{}
	graffities map[string]*graffity
	ipStates   map[string]*ipState
}

func newServerState(stateFile string) *serverState {
	return &serverState{
		stateFile:  stateFile,
		clients:    make(map[*clientConn]struct{}),
		graffities: make(map[string]*graffity),
		ipStates:   make(map[string]*ipState),
	}
}

func (s *serverState) registerClient(c *clientConn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.clients[c] = struct{}{}
}

func (s *serverState) removeClient(c *clientConn) {
	s.mu.Lock()
	delete(s.clients, c)

	affectedServers := make(map[string]struct{})
	for id, graff := range s.graffities {
		if graff.Owner == c {
			affectedServers[graff.ServerAddress] = struct{}{}
			delete(s.graffities, id)
		}
	}
	s.writeStateLocked()

	recipients := s.snapshotRecipientsLocked(affectedServers)
	s.mu.Unlock()

	for serverAddress, clients := range recipients {
		s.broadcastSnapshot(serverAddress, clients)
	}

	s.releaseConnection(c.remoteIP)
}

func (s *serverState) handleHello(c *clientConn, msg inboundMessage) {
	serverAddress := strings.TrimSpace(msg.ServerAddress)
	if serverAddress == "" {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: server_address is required"})
		return
	}
	if len(serverAddress) > maxServerAddrLen {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: server_address too long"})
		return
	}
	ownerID := strings.TrimSpace(msg.OwnerID)
	if ownerID == "" {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: owner_id is required"})
		return
	}
	if len(ownerID) > maxOwnerIDLen {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: owner_id too long"})
		return
	}

	s.mu.Lock()
	oldServer := c.serverAddress
	oldOwnerID := c.ownerID
	c.serverAddress = serverAddress
	c.ownerID = ownerID
	if oldServer != "" && (oldServer != serverAddress || oldOwnerID != ownerID) {
		for id, graff := range s.graffities {
			if graff.Owner == c || (graff.ServerAddress == oldServer && graff.OwnerID == oldOwnerID) {
				delete(s.graffities, id)
			}
		}
		s.writeStateLocked()
	}
	for _, graff := range s.graffities {
		if graff.ServerAddress == serverAddress && graff.OwnerID == ownerID {
			graff.Owner = c
		}
	}
	s.mu.Unlock()

	if oldServer != "" && oldServer != serverAddress {
		s.broadcastSnapshot(oldServer, nil)
	}
	s.sendSnapshot(c)
}

func (s *serverState) handlePlace(c *clientConn, msg inboundMessage) {
	if c.serverAddress == "" {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: hello required before place"})
		return
	}
	if len(msg.GraffityID) == 0 || len(msg.GraffityID) > maxGraffityIDLen {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: invalid graffity_id"})
		return
	}
	if _, ok := allowedGraffityIDs[msg.GraffityID]; !ok {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: unknown graffity_id"})
		return
	}
	if msg.X < -maxAbsCoordinate || msg.X > maxAbsCoordinate || msg.Y < -maxAbsCoordinate || msg.Y > maxAbsCoordinate {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: coordinates out of range"})
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	size := msg.Size
	if size < 1 {
		size = 1
	} else if size > 6 {
		size = 6
	}

	placedByIP := 0
	for _, graff := range s.graffities {
		if graff.Owner != nil && graff.Owner.remoteIP == c.remoteIP {
			placedByIP++
		}
		if graff.ServerAddress == c.serverAddress && overlaps(msg.X, msg.Y, size, graff.X, graff.Y, graff.Size) {
			_ = c.send(outboundMessage{Type: "error", Message: "Graffity: too close to another graffity"})
			return
		}
	}

	if placedByIP >= maxPerIP {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: max 3 per IP"})
		return
	}

	id := fmt.Sprintf("g-%d", atomic.AddUint64(&s.nextID, 1))
	s.graffities[id] = &graffity{
		ID:            id,
		GraffityID:    msg.GraffityID,
		ServerAddress: c.serverAddress,
		OwnerID:       c.ownerID,
		X:             msg.X,
		Y:             msg.Y,
		Size:          size,
		Air:           msg.Air,
		Owner:         c,
	}
	s.writeStateLocked()
	go s.broadcastSnapshot(c.serverAddress, nil)
}

func (s *serverState) handleRemove(c *clientConn, msg inboundMessage) {
	if msg.ID == "" {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: id is required"})
		return
	}
	if len(msg.ID) > maxEntityIDLen {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: invalid id"})
		return
	}

	s.mu.Lock()
	graff, ok := s.graffities[msg.ID]
	if !ok {
		s.mu.Unlock()
		return
	}
	if graff.ServerAddress != c.serverAddress || graff.OwnerID != c.ownerID {
		s.mu.Unlock()
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: remove denied"})
		return
	}
	graff.Owner = c

	serverAddress := graff.ServerAddress
	delete(s.graffities, msg.ID)
	s.writeStateLocked()
	s.mu.Unlock()

	s.broadcastSnapshot(serverAddress, nil)
}

func (s *serverState) sendSnapshot(c *clientConn) {
	s.mu.Lock()
	payload := s.snapshotForClientLocked(c)
	s.mu.Unlock()
	_ = c.send(payload)
}

func (s *serverState) broadcastSnapshot(serverAddress string, recipients []*clientConn) {
	s.mu.Lock()
	if recipients == nil {
		for client := range s.clients {
			if client.serverAddress == serverAddress {
				recipients = append(recipients, client)
			}
		}
	}
	payloads := make(map[*clientConn]outboundMessage, len(recipients))
	for _, client := range recipients {
		payloads[client] = s.snapshotForClientLocked(client)
	}
	s.mu.Unlock()

	for client, payload := range payloads {
		_ = client.send(payload)
	}
}

func (s *serverState) snapshotForClientLocked(c *clientConn) outboundMessage {
	out := outboundMessage{Type: "snapshot"}
	for _, graff := range s.graffities {
		if graff.ServerAddress != c.serverAddress {
			continue
		}
		out.Graffities = append(out.Graffities, outboundGraffity{
			ID:         graff.ID,
			GraffityID: graff.GraffityID,
			X:          graff.X,
			Y:          graff.Y,
			Size:       graff.Size,
			Air:        graff.Air,
			Owned:      graff.OwnerID == c.ownerID,
		})
	}
	return out
}

func (s *serverState) snapshotRecipientsLocked(affectedServers map[string]struct{}) map[string][]*clientConn {
	recipients := make(map[string][]*clientConn)
	for client := range s.clients {
		if _, ok := affectedServers[client.serverAddress]; ok {
			recipients[client.serverAddress] = append(recipients[client.serverAddress], client)
		}
	}
	return recipients
}

func (s *serverState) writeStateLocked() {
	snapshot := persistedState{
		UpdatedAt: time.Now().UTC(),
	}
	for _, graff := range s.graffities {
		snapshot.Graffities = append(snapshot.Graffities, persistedGraffity{
			ID:            graff.ID,
			GraffityID:    graff.GraffityID,
			ServerAddress: graff.ServerAddress,
			OwnerID:       graff.OwnerID,
			X:             graff.X,
			Y:             graff.Y,
			Size:          graff.Size,
			Air:           graff.Air,
		})
	}

	data, err := json.MarshalIndent(snapshot, "", "  ")
	if err != nil {
		log.Printf("marshal state: %v", err)
		return
	}

	if err := os.MkdirAll(filepath.Dir(s.stateFile), 0o755); err != nil && filepath.Dir(s.stateFile) != "." {
		log.Printf("mkdir state dir: %v", err)
		return
	}
	if err := os.WriteFile(s.stateFile, data, 0o644); err != nil {
		log.Printf("write state: %v", err)
	}
}

func overlaps(ax, ay, asize, bx, by, bsize int) bool {
	dx := ax - bx
	if dx < 0 {
		dx = -dx
	}
	dy := ay - by
	if dy < 0 {
		dy = -dy
	}
	halfExtent := graffitySize * (asize + bsize) / 2
	return dx <= halfExtent && dy <= halfExtent
}

func consumeBucket(Bucket *tokenBucket, Rate, Burst float64, Now time.Time, Cost float64) bool {
	if Bucket.Last.IsZero() {
		Bucket.Last = Now
		Bucket.Tokens = Burst
	}
	Delta := Now.Sub(Bucket.Last).Seconds()
	if Delta > 0 {
		Bucket.Tokens += Delta * Rate
		if Bucket.Tokens > Burst {
			Bucket.Tokens = Burst
		}
		Bucket.Last = Now
	}
	if Bucket.Tokens < Cost {
		return false
	}
	Bucket.Tokens -= Cost
	return true
}

func (s *serverState) ipStateLocked(IP string, Now time.Time) *ipState {
	State := s.ipStates[IP]
	if State == nil {
		State = &ipState{}
		s.ipStates[IP] = State
	}
	State.LastSeen = Now
	return State
}

func (s *serverState) pruneIPStatesLocked(Now time.Time) {
	for IP, State := range s.ipStates {
		if State.ActiveConns == 0 && Now.Sub(State.LastSeen) > ipStateTTL {
			delete(s.ipStates, IP)
		}
	}
}

func (s *serverState) tryAcceptConnection(IP string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	Now := time.Now()
	s.pruneIPStatesLocked(Now)
	State := s.ipStateLocked(IP, Now)
	if State.ActiveConns >= maxActiveConnPerIP {
		return false
	}
	if !consumeBucket(&State.ConnectLimiter, connectRatePerSecond, connectBurst, Now, 1) {
		return false
	}
	State.ActiveConns++
	return true
}

func (s *serverState) releaseConnection(IP string) {
	s.mu.Lock()
	defer s.mu.Unlock()

	Now := time.Now()
	if State := s.ipStates[IP]; State != nil {
		if State.ActiveConns > 0 {
			State.ActiveConns--
		}
		State.LastSeen = Now
	}
	s.pruneIPStatesLocked(Now)
}

func (s *serverState) allowMessage(IP string, MsgType string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	Now := time.Now()
	State := s.ipStateLocked(IP, Now)
	if !consumeBucket(&State.MessageLimiter, messageRatePerSecond, messageBurst, Now, 1) {
		return false
	}
	switch MsgType {
	case "place":
		return consumeBucket(&State.PlaceLimiter, placeRatePerSecond, placeBurst, Now, 1)
	case "remove":
		return consumeBucket(&State.RemoveLimiter, removeRatePerSecond, removeBurst, Now, 1)
	default:
		return true
	}
}

func remoteIP(addr net.Addr) string {
	host, _, err := net.SplitHostPort(addr.String())
	if err != nil {
		return addr.String()
	}
	return host
}

func handleClient(s *serverState, conn net.Conn) {
	defer conn.Close()

	client := &clientConn{
		conn:     conn,
		remoteIP: remoteIP(conn.RemoteAddr()),
	}
	s.registerClient(client)
	defer s.removeClient(client)

	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 0, 512), maxLineBytes)
	_ = conn.SetReadDeadline(time.Now().Add(helloTimeout))
	helloSeen := false
	for scanner.Scan() {
		var msg inboundMessage
		if err := json.Unmarshal(scanner.Bytes(), &msg); err != nil {
			_ = client.send(outboundMessage{Type: "error", Message: "Graffity: invalid JSON"})
			continue
		}
		msg.Type = strings.TrimSpace(msg.Type)
		if msg.Type == "" {
			_ = client.send(outboundMessage{Type: "error", Message: "Graffity: type is required"})
			continue
		}
		if !helloSeen && msg.Type != "hello" {
			_ = client.send(outboundMessage{Type: "error", Message: "Graffity: hello must be first"})
			return
		}
		if !s.allowMessage(client.remoteIP, msg.Type) {
			log.Printf("rate limit: ip=%s type=%s", client.remoteIP, msg.Type)
			_ = client.send(outboundMessage{Type: "error", Message: "Graffity: rate limit exceeded"})
			return
		}

		switch msg.Type {
		case "hello":
			s.handleHello(client, msg)
			helloSeen = true
			_ = conn.SetReadDeadline(time.Now().Add(idleTimeout))
		case "place":
			s.handlePlace(client, msg)
			_ = conn.SetReadDeadline(time.Now().Add(idleTimeout))
		case "remove":
			s.handleRemove(client, msg)
			_ = conn.SetReadDeadline(time.Now().Add(idleTimeout))
		default:
			_ = client.send(outboundMessage{Type: "error", Message: "Graffity: unknown message type"})
		}
	}

	if err := scanner.Err(); err != nil {
		if ne, ok := err.(net.Error); ok && ne.Timeout() {
			log.Printf("client %s timeout", client.remoteIP)
		} else {
			log.Printf("client %s scanner error: %v", client.remoteIP, err)
		}
	}
}

func main() {
	bindAddress := defaultBindAddress
	if len(os.Args) >= 2 && strings.TrimSpace(os.Args[1]) != "" {
		bindAddress = os.Args[1]
	}

	stateFile := defaultStateFile
	if len(os.Args) >= 3 && strings.TrimSpace(os.Args[2]) != "" {
		stateFile = os.Args[2]
	}

	listener, err := net.Listen("tcp", bindAddress)
	if err != nil {
		log.Fatalf("listen %s: %v", bindAddress, err)
	}
	defer listener.Close()

	log.Printf("graffity server listening on %s", bindAddress)
	log.Printf("writing current state to %s", stateFile)

	state := newServerState(stateFile)
	state.mu.Lock()
	state.writeStateLocked()
	state.mu.Unlock()

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		if !state.tryAcceptConnection(remoteIP(conn.RemoteAddr())) {
			log.Printf("rejecting connection from %s", remoteIP(conn.RemoteAddr()))
			_ = conn.Close()
			continue
		}
		go handleClient(state, conn)
	}
}
