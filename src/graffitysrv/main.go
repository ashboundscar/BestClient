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
	defaultBindAddress = "127.0.0.1:48365"
	defaultStateFile   = "graffity_state.json"
	maxPerIP           = 3
	graffitySize       = 32
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
	return json.NewEncoder(c.conn).Encode(v)
}

type serverState struct {
	mu         sync.Mutex
	nextID     uint64
	stateFile  string
	clients    map[*clientConn]struct{}
	graffities map[string]*graffity
}

func newServerState(stateFile string) *serverState {
	return &serverState{
		stateFile:  stateFile,
		clients:    make(map[*clientConn]struct{}),
		graffities: make(map[string]*graffity),
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
}

func (s *serverState) handleHello(c *clientConn, msg inboundMessage) {
	serverAddress := strings.TrimSpace(msg.ServerAddress)
	if serverAddress == "" {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: server_address is required"})
		return
	}
	ownerID := strings.TrimSpace(msg.OwnerID)
	if ownerID == "" {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: owner_id is required"})
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
	if _, ok := allowedGraffityIDs[msg.GraffityID]; !ok {
		_ = c.send(outboundMessage{Type: "error", Message: "Graffity: unknown graffity_id"})
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
	scanner.Buffer(make([]byte, 0, 4096), 1024*1024)
	for scanner.Scan() {
		var msg inboundMessage
		if err := json.Unmarshal(scanner.Bytes(), &msg); err != nil {
			_ = client.send(outboundMessage{Type: "error", Message: "Graffity: invalid JSON"})
			continue
		}

		switch msg.Type {
		case "hello":
			s.handleHello(client, msg)
		case "place":
			s.handlePlace(client, msg)
		case "remove":
			s.handleRemove(client, msg)
		default:
			_ = client.send(outboundMessage{Type: "error", Message: "Graffity: unknown message type"})
		}
	}

	if err := scanner.Err(); err != nil {
		log.Printf("client %s scanner error: %v", client.remoteIP, err)
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
		go handleClient(state, conn)
	}
}
