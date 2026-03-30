// Signal Space Miner — WebSocket relay server
// Binary protocol relay for multiplayer state synchronization.
package main

import (
	"encoding/binary"
	"fmt"
	"log"
	"math"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// Protocol message types.
const (
	MsgJoin            = 0x01
	MsgLeave           = 0x02
	MsgState           = 0x03
	MsgInput           = 0x04
	MsgAsteroidUpdate  = 0x05
	MsgWorldAsteroids  = 0x10
	MsgWorldNpcs       = 0x11
	MsgWorldStations   = 0x12
	MsgMiningAction    = 0x13
	MsgHostAssign      = 0x14
)

// Limits.
const (
	MaxPlayers = 32
	TickRate   = 20 // Hz
)

// PlayerState holds the latest known state for a connected player.
type PlayerState struct {
	X, Y   float32
	VX, VY float32
	Angle  float32
}

// Player represents a connected client.
type Player struct {
	ID    uint8
	Conn  *websocket.Conn
	State PlayerState
	mu    sync.Mutex
}

// Room is the single default game room.
type Room struct {
	mu      sync.RWMutex
	players map[uint8]*Player
	nextID  uint8
	hostID  uint8
	hasHost bool
}

func NewRoom() *Room {
	return &Room{
		players: make(map[uint8]*Player),
	}
}

// allocateID finds the next available player ID (0-31).
func (r *Room) allocateID() (uint8, bool) {
	for i := 0; i < MaxPlayers; i++ {
		id := (r.nextID + uint8(i)) % MaxPlayers
		if _, exists := r.players[id]; !exists {
			r.nextID = (id + 1) % MaxPlayers
			return id, true
		}
	}
	return 0, false
}

// Join adds a player to the room and returns the assigned ID.
func (r *Room) Join(conn *websocket.Conn) (*Player, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	id, ok := r.allocateID()
	if !ok {
		return nil, fmt.Errorf("room full")
	}

	p := &Player{
		ID:   id,
		Conn: conn,
	}
	r.players[id] = p

	// Send JOIN to the new player with their assigned ID.
	joinMsg := []byte{MsgJoin, id}
	if err := conn.WriteMessage(websocket.BinaryMessage, joinMsg); err != nil {
		delete(r.players, id)
		return nil, err
	}

	// Notify existing players about the new player.
	notifyMsg := []byte{MsgJoin, id}
	for pid, other := range r.players {
		if pid == id {
			continue
		}
		other.mu.Lock()
		_ = other.Conn.WriteMessage(websocket.BinaryMessage, notifyMsg)
		other.mu.Unlock()

		// Also tell the new player about existing players.
		existMsg := []byte{MsgJoin, pid}
		_ = conn.WriteMessage(websocket.BinaryMessage, existMsg)
	}

	// Host assignment: first player becomes the host.
	if !r.hasHost {
		r.hostID = id
		r.hasHost = true
		hostMsg := []byte{MsgHostAssign, 1}
		_ = conn.WriteMessage(websocket.BinaryMessage, hostMsg)
		log.Printf("player %d assigned as host", id)
	} else {
		// Tell this player they are NOT the host.
		notHostMsg := []byte{MsgHostAssign, 0}
		_ = conn.WriteMessage(websocket.BinaryMessage, notHostMsg)
	}

	log.Printf("player %d joined (%d total)", id, len(r.players))
	return p, nil
}

// Leave removes a player from the room and notifies others.
func (r *Room) Leave(p *Player) {
	r.mu.Lock()
	defer r.mu.Unlock()

	delete(r.players, p.ID)

	leaveMsg := []byte{MsgLeave, p.ID}
	for _, other := range r.players {
		other.mu.Lock()
		_ = other.Conn.WriteMessage(websocket.BinaryMessage, leaveMsg)
		other.mu.Unlock()
	}

	// If the leaving player was the host, reassign to another player.
	if r.hasHost && r.hostID == p.ID {
		r.hasHost = false
		for _, other := range r.players {
			r.hostID = other.ID
			r.hasHost = true
			other.mu.Lock()
			hostMsg := []byte{MsgHostAssign, 1}
			_ = other.Conn.WriteMessage(websocket.BinaryMessage, hostMsg)
			other.mu.Unlock()
			log.Printf("player %d reassigned as host", other.ID)
			break
		}
		if !r.hasHost {
			log.Printf("no players remaining, no host")
		}
	}

	log.Printf("player %d left (%d total)", p.ID, len(r.players))
}

// BroadcastStates sends all player states to all players.
func (r *Room) BroadcastStates() {
	r.mu.RLock()
	defer r.mu.RUnlock()

	if len(r.players) == 0 {
		return
	}

	// Build state messages for each player.
	// Each state message: 1 byte type + 1 byte id + 5 floats (20 bytes) = 22 bytes
	msgs := make([][]byte, 0, len(r.players))
	for _, p := range r.players {
		buf := make([]byte, 22)
		buf[0] = MsgState
		buf[1] = p.ID
		p.mu.Lock()
		binary.LittleEndian.PutUint32(buf[2:6], math.Float32bits(p.State.X))
		binary.LittleEndian.PutUint32(buf[6:10], math.Float32bits(p.State.Y))
		binary.LittleEndian.PutUint32(buf[10:14], math.Float32bits(p.State.VX))
		binary.LittleEndian.PutUint32(buf[14:18], math.Float32bits(p.State.VY))
		binary.LittleEndian.PutUint32(buf[18:22], math.Float32bits(p.State.Angle))
		p.mu.Unlock()
		msgs = append(msgs, buf)
	}

	// Send all state messages to all players.
	for _, p := range r.players {
		p.mu.Lock()
		for _, msg := range msgs {
			_ = p.Conn.WriteMessage(websocket.BinaryMessage, msg)
		}
		p.mu.Unlock()
	}
}

// HandleInput processes an INPUT message from a player.
func (r *Room) HandleInput(p *Player, data []byte) {
	// INPUT: 1 byte type + 1 byte flags + 1 float angle = 6 bytes
	if len(data) < 6 {
		return
	}

	flags := data[1]
	angle := math.Float32frombits(binary.LittleEndian.Uint32(data[2:6]))

	// Derive approximate velocity/position updates from flags.
	// The authoritative state comes from the client STATE messages,
	// but we store the angle from input for interpolation.
	p.mu.Lock()
	p.State.Angle = angle
	_ = flags // Flags are available for future server-side simulation.
	p.mu.Unlock()
}

// HandleState processes a STATE message from a player (client-authoritative).
func (r *Room) HandleState(p *Player, data []byte) {
	// STATE: 1 byte type + 1 byte id + 5 floats = 22 bytes
	if len(data) < 22 {
		return
	}

	p.mu.Lock()
	p.State.X = math.Float32frombits(binary.LittleEndian.Uint32(data[2:6]))
	p.State.Y = math.Float32frombits(binary.LittleEndian.Uint32(data[6:10]))
	p.State.VX = math.Float32frombits(binary.LittleEndian.Uint32(data[10:14]))
	p.State.VY = math.Float32frombits(binary.LittleEndian.Uint32(data[14:18]))
	p.State.Angle = math.Float32frombits(binary.LittleEndian.Uint32(data[18:22]))
	p.mu.Unlock()
}

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true // Allow all origins for game clients.
	},
}

var room = NewRoom()

func handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("upgrade error: %v", err)
		return
	}
	defer conn.Close()

	player, err := room.Join(conn)
	if err != nil {
		log.Printf("join error: %v", err)
		return
	}
	defer room.Leave(player)

	// Read loop: process incoming messages from this player.
	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			break
		}
		if len(data) < 1 {
			continue
		}

		switch data[0] {
		case MsgInput:
			room.HandleInput(player, data)
		case MsgState:
			room.HandleState(player, data)
		case MsgAsteroidUpdate:
			// Broadcast asteroid updates to other players.
			room.mu.RLock()
			for _, other := range room.players {
				if other.ID == player.ID {
					continue
				}
				other.mu.Lock()
				_ = other.Conn.WriteMessage(websocket.BinaryMessage, data)
				other.mu.Unlock()
			}
			room.mu.RUnlock()

		case MsgWorldAsteroids, MsgWorldNpcs, MsgWorldStations:
			// Host broadcasts world state to all other players.
			room.mu.RLock()
			for _, other := range room.players {
				if other.ID == player.ID {
					continue
				}
				other.mu.Lock()
				_ = other.Conn.WriteMessage(websocket.BinaryMessage, data)
				other.mu.Unlock()
			}
			room.mu.RUnlock()

		case MsgMiningAction:
			// Guest sends mining action — relay to host only.
			room.mu.RLock()
			if room.hasHost {
				if host, ok := room.players[room.hostID]; ok && host.ID != player.ID {
					host.mu.Lock()
					_ = host.Conn.WriteMessage(websocket.BinaryMessage, data)
					host.mu.Unlock()
				}
			}
			room.mu.RUnlock()
		}
	}
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	room.mu.RLock()
	count := len(room.players)
	room.mu.RUnlock()
	fmt.Fprintf(w, `{"status":"ok","players":%d}`, count)
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	// Start the tick loop for state broadcasting.
	go func() {
		ticker := time.NewTicker(time.Second / TickRate)
		defer ticker.Stop()
		for range ticker.C {
			room.BroadcastStates()
		}
	}()

	http.HandleFunc("/ws", handleWebSocket)
	http.HandleFunc("/health", handleHealth)

	addr := ":" + port
	log.Printf("signal relay server starting on %s", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("server error: %v", err)
	}
}
