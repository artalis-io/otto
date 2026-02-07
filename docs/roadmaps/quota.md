# Quota - Rate Quoting Engine

### Overview

**QUOTA** (**Q**uote **U**nderwriting and **T**ariff **O**ptimization **A**lgorithm) generates spot and contract pricing for outbound freight.

### Core Problem

Carriers need to quote rates quickly and accurately:
- **Spot quotes**: Immediate pricing for one-off loads
- **Contract rates**: Competitive RFP responses for committed lanes
- **Margin optimization**: Balance win rate vs profitability

### Cost Components

```c
typedef struct {
    double fuel_cost;          /* From FuelWise optimization */
    double driver_cost;        /* Pay per mile, per hour, per day */
    double toll_cost;          /* From Velo routing */
    double deadhead_cost;      /* Empty repositioning miles */
    double maintenance_cost;   /* Per-mile vehicle wear */
    double insurance_cost;     /* Per-mile insurance allocation */
    double overhead_cost;      /* Fixed cost allocation */
} QuoteCostBreakdown;
```

### Rate Generation

```c
typedef struct {
    QuoteCostBreakdown costs;
    double target_margin;      /* Desired profit margin (0.15 = 15%) */
    double market_rate;        /* Reference market rate for lane */
    double confidence;         /* How confident in market rate (0-1) */
    double win_probability;    /* Estimated probability of winning at this rate */
} QuoteRequest;

typedef struct {
    double floor_rate;         /* Minimum acceptable (cost + minimum margin) */
    double recommended_rate;   /* Balanced margin and win probability */
    double ceiling_rate;       /* Maximum competitive rate */
    QuoteCostBreakdown breakdown;
} QuoteResult;

QuoteResult qt_generate_quote(const QuoteRequest *request);
```

### Market Intelligence Integration

Quota can incorporate external market data:
- DAT spot rates
- Greenscreens benchmarks
- Historical lane rates
- Seasonal adjustments

### TODOs

- [ ] Define cost model API
- [ ] Implement base cost calculator
- [ ] Add margin optimization logic
- [ ] Integrate with Velo for toll/distance
- [ ] Integrate with FuelWise for fuel costs
- [ ] Add market rate adjustment factors
- [ ] Historical rate analysis
- [ ] Contract vs spot pricing strategies

---

