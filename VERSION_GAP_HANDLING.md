# Version Gap Handling in Orderbook

## What is a Version Gap?

A version gap occurs when the `fromVersion` of an incoming message doesn't match the expected version (which should be `last_to_version + 1`).

**Example:**
- Last processed message: `toVersion = 130841100`
- Expected next: `fromVersion = 130841101`
- Received: `fromVersion = 130841103`
- **Gap = 2** (messages 130841101 and 130841102 were missed)

## Gap Handling Strategy

The system uses different strategies based on gap size and context:

### 1. **Small Gaps (1-100 updates)** ✅ ACCEPTED

**Behavior:**
- Message is **accepted and processed normally**
- Warning logged: `"Version gap detected! Missing X updates. Orderbook may be stale."`
- **Why:** Small gaps are normal in high-frequency markets due to:
  - Network packet reordering
  - Temporary congestion
  - Timing issues

**Impact:** Orderbook may be slightly stale but continues updating

**Example:**
```
Last: 130841100 → Expected: 130841101 → Got: 130841103 (gap=2)
→ ACCEPTED: Small gap, continue processing
```

---

### 2. **Medium Gaps (101-5000 updates)** ⚠️ ACCEPTED WITH BASELINE ADJUSTMENT

**Behavior:**
- Message is **accepted and processed**
- **Baseline is adjusted** to accept the gap and future messages
- Warning logged: `"Large version gap (X updates). Adjusting baseline..."`

**What happens:**
- `last_to_version_` is adjusted to `fromVersion - 1`
- Next expected version becomes the current `fromVersion`
- Orderbook continues updating, but acknowledges it may be stale

**Why:** Better to continue with slightly stale data than stop completely

**Example:**
```
Last: 130841100 → Expected: 130841101 → Got: 130841250 (gap=149)
→ ACCEPTED: Adjust baseline from 130841101 to 130841249
→ Next expected: 130841250 (matches received fromVersion)
```

---

### 3. **Very Large Gaps (>5000 updates, first message only)** ❌ REJECTED

**Behavior:**
- Message is **rejected** (`return false`)
- Error logged: `"Skipping first message: fromVersion is X updates ahead. Orderbook would be corrupted."`
- **Only applies to the first WebSocket message after snapshot**

**Why:** Such a large gap after snapshot would corrupt the orderbook state

**Example:**
```
Snapshot: 130840000 → Expected: 130840001 → Got: 130846000 (gap=5999)
→ REJECTED: Too large, would corrupt orderbook
```

---

### 4. **Outdated Messages (negative gap > 100)** ❌ REJECTED

**Behavior:**
- Message is **rejected** (`return false`)
- Error logged: `"Outdated message: fromVersion is X updates behind expected. Ignoring."`

**Why:** Old messages could corrupt the current orderbook state

**Example:**
```
Last: 130841100 → Expected: 130841101 → Got: 130840900 (gap=-201)
→ REJECTED: Too outdated
```

---

## Gap Handling Flow

```
┌─────────────────────────────────────┐
│  Message Received                   │
│  fromVersion = X, toVersion = Y     │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Calculate Gap                      │
│  gap = fromVersion - expected       │
└──────────────┬──────────────────────┘
               │
       ┌───────┴───────┐
       │               │
       ▼               ▼
┌─────────────┐  ┌─────────────┐
│ First Msg?  │  │ Subsequent  │
│ (after      │  │ Messages    │
│  snapshot)  │  │             │
└──────┬──────┘  └──────┬──────┘
       │                │
       │                │
       ▼                ▼
┌─────────────────────────────────────┐
│  gap > 5000?  →  REJECT             │
│  gap > 1000?  →  WARNING + ADJUST   │
│  gap > 0?     →  ADJUST BASELINE    │
│  gap < -100?  →  REJECT (outdated)  │
│  else         →  ACCEPT             │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Apply Update to Orderbook          │
│  (if accepted)                      │
└─────────────────────────────────────┘
```

## What Happens After Gap Detection

### If Message is Accepted:
1. ✅ Update is applied to the orderbook
2. ✅ `last_to_version_` is updated to the message's `toVersion`
3. ✅ Orderbook continues processing future updates
4. ⚠️ Orderbook may be slightly stale (missing some intermediate updates)

### If Message is Rejected:
1. ❌ Update is NOT applied
2. ❌ `last_to_version_` remains unchanged
3. ✅ System continues waiting for next message
4. ✅ Orderbook state remains consistent

## Real-World Scenarios

### Scenario 1: Normal Operation
```
Update 1: 130841100 → 130841101 ✅
Update 2: 130841102 → 130841103 ✅
Update 3: 130841104 → 130841105 ✅
Gap = 1 (update 130841103 missing) → ACCEPTED
```

### Scenario 2: Network Congestion
```
Update 1: 130841100 → 130841101 ✅
[Network delay...]
Update 2: 130841105 → 130841106 ✅
Gap = 4 → ACCEPTED (small gap)
Orderbook continues, slightly stale
```

### Scenario 3: Connection Interruption
```
Update 1: 130841100 → 130841101 ✅
[Connection drops, reconnects]
Update 2: 130841250 → 130841251 ✅
Gap = 149 → ACCEPTED, baseline adjusted
Orderbook continues, acknowledges staleness
```

### Scenario 4: Corrupted Snapshot Gap
```
Snapshot: 130840000
First WS message: 130846000 → 130846001
Gap = 5999 → REJECTED
[System waits for valid message]
```

## Key Points

1. **Orderbook Continues Operating**: Gaps don't stop the system - it adapts and continues
2. **Staleness Tolerance**: Small gaps are expected and tolerated
3. **Safety First**: Very large gaps are rejected to prevent corruption
4. **Baseline Adjustment**: Medium gaps trigger adjustment to maintain continuity
5. **Logging**: All gaps are logged for monitoring and debugging

## Configuration

Current thresholds:
- **Small gap**: 1-100 updates (accepted)
- **Medium gap**: 101-5000 updates (accepted with adjustment)
- **Large gap**: >5000 updates (rejected on first message)
- **Outdated**: < -100 updates (rejected)

These thresholds balance between:
- **Reliability**: Rejecting clearly corrupt data
- **Availability**: Continuing operation with slightly stale data
- **Consistency**: Maintaining orderbook integrity

