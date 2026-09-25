// Package ulid implements Universally Unique Lexicographically Sortable
// Identifiers as described in https://github.com/ulid/spec.
//
// A ULID is 128 bits: a 48-bit big-endian millisecond Unix timestamp followed
// by 80 bits of randomness, encoded as 26 characters of Crockford base32.
package ulid

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"sync"
	"time"
)

// ULID is the 16-byte binary form of an identifier. Byte order is big-endian,
// so bytes.Compare order matches string order and creation time order.
type ULID [16]byte

const (
	// EncodedSize is the length of the canonical string form.
	EncodedSize = 26
	// MaxTime is the largest representable timestamp in milliseconds.
	MaxTime uint64 = 1<<48 - 1

	alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
)

var (
	ErrInvalidLength = errors.New("ulid: encoded ULID must be 26 characters")
	ErrInvalidChar   = errors.New("ulid: invalid character")
	ErrOverflow      = errors.New("ulid: value overflows 128 bits")
	ErrTimeRange     = errors.New("ulid: timestamp exceeds 48 bits")
	ErrMonotonic     = errors.New("ulid: monotonic random component overflow")
)

var decodeTable = func() (t [256]byte) {
	for i := range t {
		t[i] = 0xFF
	}
	for i := 0; i < len(alphabet); i++ {
		t[alphabet[i]] = byte(i)
		if c := alphabet[i]; c >= 'A' && c <= 'Z' {
			t[c+('a'-'A')] = byte(i)
		}
	}
	return t
}()

func nowMs() uint64 { return uint64(time.Now().UnixMilli()) }

func fillRandom(b []byte) {
	if _, err := rand.Read(b); err != nil {
		panic("ulid: crypto/rand failed: " + err.Error())
	}
}

// New returns a ULID for the current time with 80 bits of crypto randomness.
func New() ULID {
	var u ULID
	putTime(&u, nowMs())
	fillRandom(u[6:])
	return u
}

// FromParts builds a ULID from a millisecond timestamp and 10 random bytes.
func FromParts(ms uint64, random [10]byte) (ULID, error) {
	var u ULID
	if ms > MaxTime {
		return u, ErrTimeRange
	}
	putTime(&u, ms)
	copy(u[6:], random[:])
	return u, nil
}

func putTime(u *ULID, ms uint64) {
	u[0] = byte(ms >> 40)
	u[1] = byte(ms >> 32)
	u[2] = byte(ms >> 24)
	u[3] = byte(ms >> 16)
	u[4] = byte(ms >> 8)
	u[5] = byte(ms)
}

// Time returns the embedded Unix timestamp in milliseconds.
func (u ULID) Time() uint64 {
	return uint64(u[0])<<40 | uint64(u[1])<<32 | uint64(u[2])<<24 |
		uint64(u[3])<<16 | uint64(u[4])<<8 | uint64(u[5])
}

// Random returns the 80-bit random component.
func (u ULID) Random() (r [10]byte) {
	copy(r[:], u[6:])
	return r
}

// String returns the canonical 26-character encoding.
func (u ULID) String() string {
	var buf [EncodedSize]byte
	u.encode(&buf)
	return string(buf[:])
}

// AppendText appends the canonical encoding to b.
func (u ULID) AppendText(b []byte) ([]byte, error) {
	var buf [EncodedSize]byte
	u.encode(&buf)
	return append(b, buf[:]...), nil
}

// MarshalText implements encoding.TextMarshaler.
func (u ULID) MarshalText() ([]byte, error) {
	var buf [EncodedSize]byte
	u.encode(&buf)
	return buf[:], nil
}

// UnmarshalText implements encoding.TextUnmarshaler.
func (u *ULID) UnmarshalText(b []byte) error {
	v, err := parse(b)
	if err != nil {
		return err
	}
	*u = v
	return nil
}

func (u ULID) encode(dst *[EncodedSize]byte) {
	hi := binary.BigEndian.Uint64(u[:8])
	lo := binary.BigEndian.Uint64(u[8:])
	for i := EncodedSize - 1; i >= 0; i-- {
		dst[i] = alphabet[lo&31]
		lo = lo>>5 | hi<<59
		hi >>= 5
	}
}

// Parse decodes a canonical (case-insensitive) ULID string.
func Parse(s string) (ULID, error) { return parse([]byte(s)) }

func parse(s []byte) (ULID, error) {
	var u ULID
	if len(s) != EncodedSize {
		return u, ErrInvalidLength
	}
	var hi, lo uint64
	for i := 0; i < EncodedSize; i++ {
		v := decodeTable[s[i]]
		if v == 0xFF {
			return u, ErrInvalidChar
		}
		if i == 0 && v > 7 {
			return u, ErrOverflow
		}
		hi = hi<<5 | lo>>59
		lo = lo<<5 | uint64(v)
	}
	binary.BigEndian.PutUint64(u[:8], hi)
	binary.BigEndian.PutUint64(u[8:], lo)
	return u, nil
}

// Monotonic generates strictly increasing ULIDs and is safe for concurrent use.
//
// Within the same millisecond (or if the clock moves backwards) the previous
// random component is incremented by one instead of drawing new randomness.
type Monotonic struct {
	mu     sync.Mutex
	lastMs uint64
	last   ULID
	primed bool
}

// NewMonotonic returns a ready-to-use monotonic generator.
func NewMonotonic() *Monotonic { return &Monotonic{} }

// Next returns the next ULID, or ErrMonotonic if 2^80 IDs were requested in a
// single millisecond.
func (m *Monotonic) Next() (ULID, error) {
	now := nowMs()
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.primed && now <= m.lastMs {
		i := 15
		for ; i >= 6 && m.last[i] == 0xFF; i-- {
		}
		if i < 6 {
			return ULID{}, ErrMonotonic
		}
		m.last[i]++
		for j := i + 1; j < 16; j++ {
			m.last[j] = 0
		}
		return m.last, nil
	}
	if now > MaxTime {
		return ULID{}, ErrTimeRange
	}
	m.lastMs = now
	m.primed = true
	putTime(&m.last, now)
	fillRandom(m.last[6:])
	return m.last, nil
}
