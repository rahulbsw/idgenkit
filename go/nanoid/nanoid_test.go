package nanoid

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"os"
	"strconv"
	"strings"
	"testing"
)

func vectors(t testing.TB, name string) [][]string {
	t.Helper()
	f, err := os.Open("../../testdata/" + name)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var rows [][]string
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64*1024), 1<<20)
	for sc.Scan() {
		line := sc.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		rows = append(rows, strings.Fields(line))
	}
	return rows
}

func TestVectors(t *testing.T) {
	for _, r := range vectors(t, "nanoid.txt") {
		size, _ := strconv.Atoi(r[1])
		stream, _ := hex.DecodeString(r[2])
		g, err := CustomRandom(r[0], size, bytes.NewReader(stream))
		if err != nil {
			t.Fatal(err)
		}
		if got := g.Generate(); got != r[3] {
			t.Fatalf("alphabet %q size %d: got %s want %s", r[0], size, got, r[3])
		}
	}
}

func TestDefault(t *testing.T) {
	seen := map[string]struct{}{}
	for i := 0; i < 10000; i++ {
		id := New()
		if len(id) != DefaultSize || strings.Trim(id, URLAlphabet) != "" {
			t.Fatalf("bad id %q", id)
		}
		seen[id] = struct{}{}
	}
	if len(seen) != 10000 {
		t.Fatal("duplicates")
	}
	if id, _ := NewSize(64); len(id) != 64 {
		t.Fatal("NewSize length")
	}
}

func TestCustom(t *testing.T) {
	g, _ := CustomAlphabet("0123456789", 12)
	id := g.Generate()
	if len(id) != 12 || strings.Trim(id, "0123456789") != "" {
		t.Fatalf("bad id %q", id)
	}
	u, _ := CustomAlphabet("αβγδ", 8)
	if id := u.Generate(); len([]rune(id)) != 8 || strings.Trim(id, "αβγδ") != "" {
		t.Fatalf("bad unicode id %q", id)
	}
}

func TestDistribution(t *testing.T) {
	g, _ := CustomAlphabet("abcdefghij", 100000)
	counts := map[rune]int{}
	for _, c := range g.Generate() {
		counts[c]++
	}
	for c, n := range counts {
		if n < 9500 || n > 10500 {
			t.Fatalf("symbol %c count %d outside 5%% of 10000", c, n)
		}
	}
}

func TestValidation(t *testing.T) {
	if _, err := CustomAlphabet("", 5); err != ErrAlphabet {
		t.Fatal("empty alphabet accepted")
	}
	if _, err := CustomAlphabet(strings.Repeat("x", 257), 5); err != ErrAlphabet {
		t.Fatal("oversized alphabet accepted")
	}
	if _, err := NewSize(0); err != ErrSize {
		t.Fatal("size 0 accepted")
	}
}

var sink string

func BenchmarkNew(b *testing.B) {
	for i := 0; i < b.N; i++ {
		sink = New()
	}
}

func BenchmarkCustomHex(b *testing.B) {
	g, _ := CustomAlphabet("0123456789abcdef", DefaultSize)
	for i := 0; i < b.N; i++ {
		sink = g.Generate()
	}
}

func BenchmarkCustomAlnum(b *testing.B) {
	g, _ := CustomAlphabet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", DefaultSize)
	for i := 0; i < b.N; i++ {
		sink = g.Generate()
	}
}

func BenchmarkNewParallel(b *testing.B) {
	b.RunParallel(func(pb *testing.PB) {
		var s string
		for pb.Next() {
			s = New()
		}
		_ = s
	})
}
