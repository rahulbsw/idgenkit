# Top-level driver. Each component can also be built on its own from its directory.
PG_MAJOR ?= 17

.PHONY: test test-libs test-dbs bench docs-tables vectors \
        test-python test-go test-rust test-java test-c test-redis test-mysql test-postgres clean

test: test-libs test-dbs
test-libs: test-python test-go test-rust test-java test-c
test-dbs: test-redis test-mysql test-postgres

test-python:
	cd python && python3 -m unittest discover -s tests
test-go:
	cd go && go vet ./... && go test -race ./...
test-rust:
	cd rust && cargo test --release
test-java:
	$(MAKE) -C java test
test-c:
	$(MAKE) -C c test
test-redis:
	$(MAKE) -C redis test
test-mysql:
	$(MAKE) -C mysql test
test-postgres:
	PG_MAJOR=$(PG_MAJOR) ./postgres/test.sh

bench:
	./bench/run_all.sh

docs-tables:
	python3 bench/doc_tables.py

vectors:
	python3 testdata/generate_vectors.py

clean:
	$(MAKE) -C c clean
	$(MAKE) -C java clean
	$(MAKE) -C redis clean
	$(MAKE) -C mysql clean
	cd rust && cargo clean
