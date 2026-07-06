-- Enable TimescaleDB extension
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- 1. Market Data Table (Ticks)
-- Optimized for massive ingest rate
CREATE TABLE IF NOT EXISTS market_ticks (
    time        TIMESTAMPTZ NOT NULL,
    symbol      TEXT NOT NULL,
    price       DOUBLE PRECISION NOT NULL,
    volume      DOUBLE PRECISION NOT NULL,
    side        CHAR(1) NOT NULL, -- 'B'uy or 'S'ell
    source      TEXT NOT NULL
);

-- Convert to Hypertable (Partition by time, 1-day chunks)
SELECT create_hypertable('market_ticks', 'time', chunk_time_interval => INTERVAL '1 day');

-- Indexes for fast retrieval
CREATE INDEX ON market_ticks (symbol, time DESC);

-- 2. OHLCV Candles (Aggregated Data)
CREATE TABLE IF NOT EXISTS market_candles (
    time        TIMESTAMPTZ NOT NULL,
    symbol      TEXT NOT NULL,
    resolution  TEXT NOT NULL, -- '1m', '5m', '1h'
    open        DOUBLE PRECISION NOT NULL,
    high        DOUBLE PRECISION NOT NULL,
    low         DOUBLE PRECISION NOT NULL,
    close       DOUBLE PRECISION NOT NULL,
    volume      DOUBLE PRECISION NOT NULL
);

SELECT create_hypertable('market_candles', 'time', chunk_time_interval => INTERVAL '1 week');
CREATE INDEX ON market_candles (symbol, resolution, time DESC);

-- 3. Order Audit Log
CREATE TABLE IF NOT EXISTS order_audit (
    order_id    UUID NOT NULL,
    client_id   TEXT NOT NULL,
    symbol      TEXT NOT NULL,
    side        TEXT NOT NULL,
    status      TEXT NOT NULL,
    price       DOUBLE PRECISION,
    quantity    DOUBLE PRECISION,
    created_at  TIMESTAMPTZ DEFAULT NOW(),
    updated_at  TIMESTAMPTZ DEFAULT NOW()
);
