// Package uuid implements RFC 9562 version 4 (random) and version 7
// (time-ordered) UUIDs. The Go standard library has no UUID type.
//
// A UUIDv7 is a 48-bit big-endian millisecond Unix timestamp, the version
// nibble, 12 random bits, the variant bits and 62 more random bits. A UUIDv4
// is 122 random bits plus the version and variant bits.
package uuid

import (
	"crypto/rand"
	"errors"
	"sync"
	"time"
)

// UUID is the 16-byte binary form, in the same byte order as the text form,
// so bytes.Compare order matches string order.
type UUID [16]byte

const (
	// EncodedSize is the length of the canonical string form.
	EncodedSize = 36
	// MaxTime is the largest UUIDv7 timestamp in milliseconds.
	MaxTime uint64 = 1<<48 - 1

	randBMax  = 1<<62 - 1
	hexDigits = "0123456789abcdef"
)

var (
	ErrInvalidFormat = errors.New("uuid: must be 36 characters in 8-4-4-4-12 hex form")
	ErrTimeRange     = errors.New("uuid: timestamp exceeds 48 bits")
	ErrMonotonic     = errors.New("uuid: monotonic random component overflow")
	ErrNotV7         = errors.New("uuid: not a version 7 UUID")
)

func nowMs() uint64 { return uint64(time.Now().UnixMilli()) }

func fillRandom(b []byte) {
	if _, err := rand.Read(b); err != nil {
		panic("uuid: crypto/rand failed: " + err.Error())
	}
}

func (u *UUID) setVersion(v byte) {
	u[6] = u[6]&0x0F | v<<4
	u[8] = u[8]&0x3F | 0x80
}

// NewV4 returns a random (version 4) UUID.
func NewV4() UUID {
	var u UUID
	fillRandom(u[:])
	u.setVersion(4)
	return u
}

// V4FromRandom sets the version and variant bits of 16 random bytes.
func V4FromRandom(random [16]byte) UUID {
	u := UUID(random)
	u.setVersion(4)
	return u
}

// NewV7 returns a time-ordered (version 7) UUID for the current time.
func NewV7() UUID {
	var rnd [10]byte
	fillRandom(rnd[:])
	u, err := V7FromParts(nowMs(), rnd)
	if err != nil {
		panic(err)
	}
	return u
}

// V7FromParts builds a UUIDv7 from a millisecond timestamp and 10 random
// bytes; the version and variant bits overwrite 6 of the random bits.
func V7FromParts(ms uint64, random [10]byte) (UUID, error) {
	var u UUID
	if ms > MaxTime {
		return u, ErrTimeRange
	}
	for i := 5; i >= 0; i-- {
		u[i] = byte(ms)
		ms >>= 8
	}
	copy(u[6:], random[:])
	u.setVersion(7)
	return u, nil
}

// Version returns the version nibble (4 for NewV4, 7 for NewV7).
func (u UUID) Version() int { return int(u[6] >> 4) }

// Time returns the Unix timestamp in milliseconds of a version 7 UUID.
func (u UUID) Time() (uint64, error) {
	if u.Version() != 7 {
		return 0, ErrNotV7
	}
	var ms uint64
	for i := 0; i < 6; i++ {
		ms = ms<<8 | uint64(u[i])
	}
	return ms, nil
}

// String returns the canonical lowercase 36-character form.
func (u UUID) String() string {
	var buf [EncodedSize]byte
	u.encode(&buf)
	return string(buf[:])
}

// AppendText appends the canonical encoding to b.
func (u UUID) AppendText(b []byte) ([]byte, error) {
	var buf [EncodedSize]byte
	u.encode(&buf)
	return append(b, buf[:]...), nil
}

// MarshalText implements encoding.TextMarshaler.
func (u UUID) MarshalText() ([]byte, error) {
	var buf [EncodedSize]byte
	u.encode(&buf)
	return buf[:], nil
}

// UnmarshalText implements encoding.TextUnmarshaler.
func (u *UUID) UnmarshalText(b []byte) error {
	v, err := parse(b)
	if err != nil {
		return err
	}
	*u = v
	return nil
}

func (u UUID) encode(dst *[EncodedSize]byte) {
	o := 0
	for i, b := range u {
		if i == 4 || i == 6 || i == 8 || i == 10 {
			dst[o] = '-'
			o++
		}
		dst[o], dst[o+1] = hexDigits[b>>4], hexDigits[b&15]
		o += 2
	}
}

// Parse decodes the case-insensitive 8-4-4-4-12 form. Braces, the urn:uuid:
// prefix and the 32-digit form without hyphens are rejected.
func Parse(s string) (UUID, error) { return parse([]byte(s)) }

func parse(s []byte) (UUID, error) {
	var u UUID
	if len(s) != EncodedSize {
		return u, ErrInvalidFormat
	}
	t := 0
	for i := range u {
		if t == 8 || t == 13 || t == 18 || t == 23 {
			if s[t] != '-' {
				return UUID{}, ErrInvalidFormat
			}
			t++
		}
		hi, lo := hexValue(s[t]), hexValue(s[t+1])
		if hi < 0 || lo < 0 {
			return UUID{}, ErrInvalidFormat
		}
		u[i] = byte(hi<<4 | lo)
		t += 2
	}
	return u, nil
}

func hexValue(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c|0x20 >= 'a' && c|0x20 <= 'f':
		return int(c|0x20-'a') + 10
	}
	return -1
}

// Monotonic generates strictly increasing UUIDv7s and is safe for concurrent
// use. Within the same millisecond (or if the clock moves backwards) the
// previous 74 random bits are incremented by one instead of drawing new ones.
type Monotonic struct {
	mu     sync.Mutex
	lastMs uint64
	last   UUID
	primed bool
}

// NewMonotonic returns a ready-to-use monotonic generator.
func NewMonotonic() *Monotonic { return &Monotonic{} }

// Next returns the next UUIDv7, or ErrMonotonic if 2^74 IDs were requested in
// a single millisecond.
func (m *Monotonic) Next() (UUID, error) {
	return m.next(nowMs(), fillRandom)
}

func (m *Monotonic) next(now uint64, fill func([]byte)) (UUID, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.primed && now <= m.lastMs {
		next, ok := increment(m.last)
		if !ok {
			return UUID{}, ErrMonotonic
		}
		m.last = next
		return next, nil
	}
	if now > MaxTime {
		return UUID{}, ErrTimeRange
	}
	var rnd [10]byte
	fill(rnd[:])
	m.last, _ = V7FromParts(now, rnd)
	m.lastMs = now
	m.primed = true
	return m.last, nil
}

// increment adds one to the 74 random bits (rand_a<<62 | rand_b), skipping
// the version and variant bits.
func increment(u UUID) (UUID, bool) {
	randA := uint64(u[6]&0x0F)<<8 | uint64(u[7])
	randB := uint64(u[8] & 0x3F)
	for i := 9; i < 16; i++ {
		randB = randB<<8 | uint64(u[i])
	}
	switch {
	case randB < randBMax:
		randB++
	case randA < 0xFFF:
		randA, randB = randA+1, 0
	default:
		return u, false
	}
	u[6], u[7] = 0x70|byte(randA>>8), byte(randA)
	for i := 15; i >= 9; i-- {
		u[i] = byte(randB)
		randB >>= 8
	}
	u[8] = 0x80 | byte(randB)
	return u, true
}
