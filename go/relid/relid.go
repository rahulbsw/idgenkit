// Package relid implements relative IDs: sortable, unique IDs in which every
// ID generated for the same key starts with the same tag.
//
//	3F8KQZ-01J8Y4Q9M3-X3B0N6E3P8
//	tag    48-bit ms  50-bit random / counter
//
// The tag is the top 30 bits of HMAC-SHA-256(secret, BE32(len(salt)) ||
// salt || key). The secret keeps tags unguessable; the optional salt gives the
// same key unrelated tags in different contexts.
package relid

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"hash"
	"hash/maphash"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	// EncodedSize is the length of the canonical text form.
	EncodedSize = 28
	// TagSize is the length of a tag's text form.
	TagSize = 6
	// MinSecretSize is the shortest accepted secret, in bytes.
	MinSecretSize = 16
	MaxTag        = 1<<30 - 1
	MaxTime       = 1<<48 - 1
	MaxRandom     = 1<<50 - 1

	alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

	// Tags of recently used keys up to cacheMaxKey bytes are cached in a
	// direct-mapped table of cacheSlots entries.
	cacheSlots  = 256
	cacheMaxKey = 64
)

var (
	ErrSecret        = errors.New("relid: secret must be at least 16 bytes")
	ErrInvalidFormat = errors.New("relid: must be 28 characters (or 26 without hyphens) of Crockford base32")
	ErrRange         = errors.New("relid: tag, timestamp or random value out of range")
	ErrMonotonic     = errors.New("relid: monotonic counter overflow")
)

var decodeTable = func() (t [256]int8) {
	for i := range t {
		t[i] = -1
	}
	for i := 0; i < len(alphabet); i++ {
		t[alphabet[i]] = int8(i)
		t[alphabet[i]|0x20] = int8(i)
	}
	return t
}()

// Parts is a decoded relative ID.
type Parts struct {
	Tag    uint32
	Time   uint64 // Unix milliseconds
	Random uint64
}

// String returns the canonical 28-character form.
func (p Parts) String() string {
	var b [EncodedSize]byte
	put(b[0:6], uint64(p.Tag))
	b[6] = '-'
	put(b[7:17], p.Time)
	b[17] = '-'
	put(b[18:28], p.Random)
	return string(b[:])
}

// TagText returns the 6-character tag every ID with this tag starts with.
func (p Parts) TagText() string { return EncodeTag(p.Tag) }

func put(dst []byte, v uint64) {
	for i := len(dst) - 1; i >= 0; i-- {
		dst[i] = alphabet[v&31]
		v >>= 5
	}
}

// EncodeTag returns the 6-character text form of a 30-bit tag.
func EncodeTag(tag uint32) string {
	var b [TagSize]byte
	put(b[:], uint64(tag&MaxTag))
	return string(b[:])
}

// FromParts encodes a tag, millisecond timestamp and 50-bit random value.
func FromParts(tag uint32, ms, random uint64) (string, error) {
	if tag > MaxTag || ms > MaxTime || random > MaxRandom {
		return "", ErrRange
	}
	return Parts{tag, ms, random}.String(), nil
}

func get(s string) (uint64, bool) {
	var v uint64
	for i := 0; i < len(s); i++ {
		d := decodeTable[s[i]]
		if d < 0 {
			return 0, false
		}
		v = v<<5 | uint64(d)
	}
	return v, true
}

// Parse decodes the case-insensitive 28-character form, or the same 26
// characters without hyphens.
func Parse(s string) (Parts, error) {
	var tag, ms, rnd string
	switch {
	case len(s) == EncodedSize && s[6] == '-' && s[17] == '-':
		tag, ms, rnd = s[0:6], s[7:17], s[18:28]
	case len(s) == EncodedSize-2:
		tag, ms, rnd = s[0:6], s[6:16], s[16:26]
	default:
		return Parts{}, ErrInvalidFormat
	}
	t, ok1 := get(tag)
	m, ok2 := get(ms)
	r, ok3 := get(rnd)
	if !ok1 || !ok2 || !ok3 || m > MaxTime {
		return Parts{}, ErrInvalidFormat
	}
	return Parts{uint32(t), m, r}, nil
}

// Generator produces relative IDs for one secret and salt. It is safe for
// concurrent use.
type Generator struct {
	macs   sync.Pool
	prefix []byte
	seed   maphash.Seed
	cache  [cacheSlots]atomic.Pointer[cachedTag]

	mu       sync.Mutex
	lastMs   uint64
	lastRand uint64
	primed   bool
}

// New returns a generator. The secret must be at least MinSecretSize bytes
// and must come from configuration or a key store, never from source code.
func New(secret []byte, salt string) (*Generator, error) {
	if len(secret) < MinSecretSize {
		return nil, ErrSecret
	}
	if uint64(len(salt)) > 0xFFFFFFFF {
		return nil, ErrRange
	}
	key := append([]byte(nil), secret...)
	prefix := binary.BigEndian.AppendUint32(nil, uint32(len(salt)))
	g := &Generator{prefix: append(prefix, salt...), seed: maphash.MakeSeed()}
	g.macs.New = func() any { return hmac.New(sha256.New, key) }
	return g, nil
}

type cachedTag struct {
	key string
	tag uint32
}

// TagValue returns the 30-bit tag for key.
func (g *Generator) TagValue(key string) uint32 {
	if len(key) > cacheMaxKey {
		return g.computeTag(key)
	}
	slot := &g.cache[maphash.String(g.seed, key)%cacheSlots]
	if c := slot.Load(); c != nil && c.key == key {
		return c.tag
	}
	tag := g.computeTag(key)
	slot.Store(&cachedTag{strings.Clone(key), tag})
	return tag
}

func (g *Generator) computeTag(key string) uint32 {
	mac := g.macs.Get().(hash.Hash)
	mac.Reset()
	mac.Write(g.prefix)
	mac.Write([]byte(key))
	var sum [sha256.Size]byte
	mac.Sum(sum[:0])
	g.macs.Put(mac)
	return binary.BigEndian.Uint32(sum[:4]) >> 2
}

// Tag returns the 6-character tag that starts every ID for key.
func (g *Generator) Tag(key string) string { return EncodeTag(g.TagValue(key)) }

func nowMs() uint64 { return uint64(time.Now().UnixMilli()) }

func fillRandom(b []byte) {
	if _, err := rand.Read(b); err != nil {
		panic("relid: crypto/rand failed: " + err.Error())
	}
}

func random50(fill func([]byte)) uint64 {
	var b [8]byte
	fill(b[1:])
	return binary.BigEndian.Uint64(b[:]) & MaxRandom
}

// Generate returns a new ID for key with 50 fresh random bits.
func (g *Generator) Generate(key string) (string, error) {
	now := nowMs()
	if now > MaxTime {
		return "", ErrRange
	}
	return Parts{g.TagValue(key), now, random50(fillRandom)}.String(), nil
}

// Monotonic returns a new ID for key from a counter shared by every key, so
// each key's IDs strictly increase and no two IDs from g are equal. It returns
// ErrMonotonic if the counter is exhausted within a single millisecond.
func (g *Generator) Monotonic(key string) (string, error) {
	return g.monotonicAt(g.TagValue(key), nowMs(), fillRandom)
}

func (g *Generator) monotonicAt(tag uint32, now uint64, fill func([]byte)) (string, error) {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.primed && now <= g.lastMs {
		if g.lastRand == MaxRandom {
			return "", ErrMonotonic
		}
		g.lastRand++
	} else {
		if now > MaxTime {
			return "", ErrRange
		}
		g.lastMs, g.lastRand, g.primed = now, random50(fill), true
	}
	return Parts{tag, g.lastMs, g.lastRand}.String(), nil
}
