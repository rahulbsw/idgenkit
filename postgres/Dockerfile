# Build context: repository root.
#   docker build -f postgres/Dockerfile --build-arg PG_MAJOR=17 -t idgenkit-pg:17 .
ARG PG_MAJOR=17
FROM postgres:${PG_MAJOR}
ARG PG_MAJOR

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential postgresql-server-dev-${PG_MAJOR} \
 && rm -rf /var/lib/apt/lists/*

COPY c /src/c
COPY testdata /src/testdata
COPY postgres /src/postgres
RUN make -C /src/postgres clean all install \
 && make -C /src/c clean test sanitize \
 && chown -R postgres:postgres /src
