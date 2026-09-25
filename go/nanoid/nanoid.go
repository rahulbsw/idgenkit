// Package nanoid generates compact, URL-friendly random string IDs following
// https://github.com/ai/nanoid. Custom alphabets use mask-based rejection
// sampling so every symbol is equally likely.
package nanoid

import (
	"crypto/rand"
	"errors"
	"io"
	"math/bits"
	"unicode/utf8"
)

const (
	// URLAlphabet is the default 64-symbol URL-safe alphabet (A-Za-z0-9_-).
	URLAlphabet = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict"
	// DefaultSize gives a collision probability similar to UUID v4.
	DefaultSize = 21
)

var (
	ErrAlphabet = errors.New("nanoid: alphabet must contain between 1 and 256 symbols")
	ErrSize     = errors.New("nanoid: size must be >= 1")
)

// New returns a DefaultSize ID using URLAlphabet.
func New() string {
	var b [DefaultSize]byte
	fill(rand.Reader, b[:])
	for i := range b {
		b[i] = URLAlphabet[b[i]&63]
	}
	return string(b[:])
}

// NewSize returns an ID of the given size using URLAlphabet.
func NewSize(size int) (string, error) {
	if size < 1 {
		return "", ErrSize
	}
	b := make([]byte, size)
	fill(rand.Reader, b)
	for i := range b {
		b[i] = URLAlphabet[b[i]&63]
	}
	return string(b), nil
}

// Generator produces IDs from a fixed alphabet. Safe for concurrent use when
// its random source is (crypto/rand.Reader is).
type Generator struct {
	ascii  []byte
	runes  []rune
	length int
	mask   byte
	size   int
	random io.Reader
}

// CustomAlphabet returns a generator drawing from crypto/rand.
func CustomAlphabet(alphabet string, size int) (*Generator, error) {
	return CustomRandom(alphabet, size, rand.Reader)
}

// CustomRandom returns a generator using the supplied random byte source.
func CustomRandom(alphabet string, size int, random io.Reader) (*Generator, error) {
	if size < 1 {
		return nil, ErrSize
	}
	g := &Generator{size: size, random: random}
	if isASCII(alphabet) {
		g.ascii = []byte(alphabet)
		g.length = len(g.ascii)
	} else {
		g.runes = []rune(alphabet)
		g.length = len(g.runes)
	}
	if g.length < 1 || g.length > 256 {
		return nil, ErrAlphabet
	}
	g.mask = byte(2<<(bits.Len32(uint32(g.length-1)|1)-1) - 1)
	return g, nil
}

// Generate returns an ID of the generator's default size.
func (g *Generator) Generate() string { return g.generate(g.size) }

// GenerateSize returns an ID of the given size.
func (g *Generator) GenerateSize(size int) (string, error) {
	if size < 1 {
		return "", ErrSize
	}
	return g.generate(size), nil
}

func (g *Generator) step(size int) int {
	if int(g.mask)+1 == g.length {
		return size
	}
	m := int(g.mask)
	return (8*m*size + 5*g.length - 1) / (5 * g.length)
}

func (g *Generator) generate(size int) string {
	buf := make([]byte, g.step(size))
	if g.ascii != nil {
		out := make([]byte, 0, size)
		for {
			fill(g.random, buf)
			for _, b := range buf {
				if i := int(b & g.mask); i < g.length {
					out = append(out, g.ascii[i])
					if len(out) == size {
						return string(out)
					}
				}
			}
		}
	}
	out := make([]rune, 0, size)
	for {
		fill(g.random, buf)
		for _, b := range buf {
			if i := int(b & g.mask); i < g.length {
				out = append(out, g.runes[i])
				if len(out) == size {
					return string(out)
				}
			}
		}
	}
}

func fill(r io.Reader, b []byte) {
	if _, err := io.ReadFull(r, b); err != nil {
		panic("nanoid: random source failed: " + err.Error())
	}
}

func isASCII(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] >= utf8.RuneSelf {
			return false
		}
	}
	return true
}
