# OTTO Strategic Business Plan

## Executive Summary

OTTO is an open-source logistics optimization platform targeting mid-to-large trucking fleets in the EU and US. The business model combines open-source core engines with proprietary integrations and managed services, addressing a market gap left by closed-source vendors who create lock-in risk for enterprise customers.

**Key thesis:** Large fleets like Girteka and Waberer's experienced vendor lock-in when Nexogen was acquired by Trimble in 2022. They want planning technology without dependency on a single vendor's roadmap. Open source provides that insurance while managed services provide the convenience.

---

## 1. Why Open Source

### 1.1 The Problem with Closed Source Planning Software

Enterprise fleets investing in planning systems face structural risks:

| Risk | Impact |
|------|--------|
| Vendor acquisition | Roadmap shifts, support degrades, pricing changes |
| Vendor bankruptcy | Software disappears, years of integration lost |
| Vendor pivots | Product sunset, forced migration |
| Pricing leverage | No alternatives once deeply integrated |
| Audit inability | Cannot verify algorithms, must trust black box |

**Case study:** Nexogen's acquisition by Trimble (2022) left European fleets uncertain about product continuity, support quality, and roadmap alignment. Fleets that had spent years integrating planning systems suddenly had no control over their technology stack.

### 1.2 What Open Source Solves

| Fear | Open Source Answer |
|------|-------------------|
| Vendor disappears | Code is MIT licensed, fork and continue |
| Rug-pull on pricing | Self-host at current capability forever |
| Roadmap diverges | Fork and extend for your specific needs |
| Key people leave | Community + documentation + hire anyone |
| Can't audit algorithms | Full source code transparency |
| Integration hostage | Own your integrations, no proprietary lock |

### 1.3 Open Source as Trust Architecture

For a fleet running 10,000+ trucks, planning software is mission-critical infrastructure. Open source transforms the vendor relationship:

- **Traditional model:** "Trust us, sign a 3-year contract, hope we don't get acquired"
- **Open source model:** "Here's the code. Pay us for support and integrations because it's convenient, not because you're trapped"

This creates genuine partnership rather than dependency.

### 1.4 Open Source as Market Validation

The best code wins in open source. If OTTO's algorithms are superior, adoption will prove it publicly. This creates:

- **Credibility:** Running in production at visible companies
- **Talent attraction:** Engineers want to work on respected open source
- **Acquisition signal:** Acquirers can audit before buying
- **Sales acceleration:** Prospects can evaluate without sales calls

---

## 2. Business Model: Open Core

### 2.1 Model Structure

```
┌─────────────────────────────────────────────────────────────┐
│                     COMMERCIAL LAYER                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │   Nexus     │  │  Managed    │  │  Premium Support    │  │
│  │ TMS/ELD     │  │  Hosting    │  │  SLAs + Dedicated   │  │
│  │ Connectors  │  │  Platform   │  │  Engineering        │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                     OPEN SOURCE (MIT)                       │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────┐  │
│  │ Ralph  │ │  Velo  │ │ Carta  │ │ Locus  │ │ FuelWise │  │
│  │ Solver │ │ Routes │ │ Tiles  │ │ Geocode│ │  Fuel    │  │
│  └────────┘ └────────┘ └────────┘ └────────┘ └──────────┘  │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────┐  │
│  │  HoSE  │ │ Tempo  │ │ Arbor  │ │ Sigma  │ │  Pulse   │  │
│  │  HoS   │ │ Time   │ │ Search │ │ Fleet  │ │  PTA     │  │
│  └────────┘ └────────┘ └────────┘ └────────┘ └──────────┘  │
│  ┌────────────────┐ ┌────────────────────────────────────┐  │
│  │   ClayShards   │ │            Shared                  │  │
│  │   UI Library   │ │   Geo, Protobuf, Compression       │  │
│  └────────────────┘ └────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 What's Open (MIT License)

All core optimization engines:

| Component | Function | Why Open |
|-----------|----------|----------|
| Ralph | LP/MIP solver | Commodity algorithm, differentiation is in application |
| Velo | Routing engine | Commodity, OSM-based |
| Carta | Map tiles | Commodity, OpenMapTiles compatible |
| Locus | Geocoding | Commodity, supports OSM data |
| FuelWise | Fuel optimization | Domain logic, shows expertise |
| HoSE | Hours of Service | Domain logic, regulatory compliance |
| Tempo | Time windows | Domain logic, scheduling rules |
| Arbor | State-space search | Algorithm framework |
| Sigma | Fleet selection | MIP-based assignment |
| Pulse | Execution tracking | PTA computation |
| ClayShards | UI components | Developer adoption |
| Shared | Utilities | Common infrastructure |

**Rationale:** These components demonstrate domain expertise and build trust. They're valuable but not sufficient alone—integration and operation are where real enterprise value lies.

### 2.3 What's Commercial

| Component | Function | Pricing Model |
|-----------|----------|---------------|
| Nexus | TMS/ELD/LoadBoard connectors | Per-connector license or subscription |
| Managed Platform | Fully hosted OTTO | Annual subscription (ARR) |
| Premium Support | SLAs, dedicated engineering | Annual contract |
| Custom Development | Fleet-specific optimization | Project-based or retainer |
| Training | Implementation, best practices | Per-engagement |

**Rationale:** Integration is where fleets need help most and where switching costs naturally exist. A Samsara ELD connector requires Samsara-specific knowledge; that's legitimate proprietary value.

---

## 3. Revenue Model

### 3.1 Pricing Structure

#### Tier 1: Self-Service (Open Source)
- **Price:** Free (MIT)
- **Includes:** All core engines, documentation, community support
- **Target:** Small fleets, developers, tire-kickers
- **Goal:** Adoption, community, brand building

#### Tier 2: Professional
- **Price:** €50-100k/year
- **Includes:** Managed hosting, standard integrations (2-3 TMS/ELD), email support
- **Target:** Mid-size fleets (50-500 trucks)
- **Goal:** Volume, market penetration

#### Tier 3: Enterprise
- **Price:** €250-500k/year
- **Includes:** Managed hosting, unlimited integrations, dedicated support, SLA, custom development hours
- **Target:** Large fleets (500+ trucks), 3PLs
- **Goal:** High-value accounts, strategic relationships

#### Tier 4: Strategic Partnership
- **Price:** €500k-1M+/year
- **Includes:** Everything in Enterprise + co-development, white-labeling, exclusive features
- **Target:** Very large fleets (5000+ trucks), technology partners
- **Goal:** Anchor accounts, reference customers

### 3.2 Revenue Projections

**Conservative scenario (5 years):**

| Year | Self-Service | Professional | Enterprise | Total ARR |
|------|--------------|--------------|------------|-----------|
| 1 | 50 users | 2 × €75k | 1 × €350k | €500k |
| 2 | 200 users | 5 × €75k | 3 × €375k | €1.5M |
| 3 | 500 users | 10 × €75k | 6 × €400k | €3.15M |
| 4 | 1000 users | 15 × €80k | 10 × €420k | €5.4M |
| 5 | 2000 users | 20 × €85k | 15 × €450k | €8.45M |

**Key assumptions:**
- Professional tier average: €75-85k/year
- Enterprise tier average: €350-450k/year
- Low churn (<5%) due to switching costs
- Modest expansion revenue (upgrades, more trucks)

### 3.3 Unit Economics

| Metric | Target |
|--------|--------|
| CAC (Customer Acquisition Cost) | <€50k for Enterprise |
| LTV (Lifetime Value) | >€1.5M (4+ years × €400k) |
| LTV:CAC Ratio | >30:1 |
| Gross Margin | 80%+ (software + hosting) |
| Net Revenue Retention | >110% (expansion) |
| Logo Churn | <5% annually |

**Why these metrics are achievable:**
- Planning software is sticky (high switching costs)
- Fleets grow/shrink trucks, revenue scales
- Once integrated, rarely ripped out
- Open source reduces sales friction (try before buy)

---

## 4. Open Source vs Closed Source: Pros and Cons

### 4.1 Advantages of Open Source

| Advantage | Impact |
|-----------|--------|
| **Trust & Transparency** | Enterprises can audit code, verify algorithms, assess security |
| **Reduced Sales Friction** | Prospects evaluate independently, shorter sales cycles |
| **Community Contributions** | Bug reports, edge cases, integrations from users |
| **Talent Acquisition** | Engineers want to work on visible, respected projects |
| **Marketing Flywheel** | GitHub stars, conference talks, word-of-mouth |
| **Strategic Positioning** | "We're confident enough to show our work" |
| **Acquisition Attractiveness** | Acquirers can diligence the code before buying |
| **Customer Lock-in Removal** | Paradoxically increases loyalty (trust-based relationship) |

### 4.2 Disadvantages of Open Source

| Disadvantage | Mitigation |
|--------------|------------|
| **Competitors can fork** | They still need domain expertise to extend/maintain |
| **Harder to capture value** | Proprietary integration layer (Nexus) |
| **Support expectations** | Clear tier separation: community vs paid support |
| **IP exposure** | Algorithms aren't the moat; judgment and integrations are |
| **Investor skepticism** | Point to Red Hat, HashiCorp, GitLab precedents |
| **Free riders** | They're still marketing; convert later or let go |

### 4.3 Comparison Matrix

| Factor | Closed Source | Open Source (OTTO) |
|--------|---------------|-------------------|
| Customer trust | Lower (black box) | Higher (auditable) |
| Sales cycle | Longer (POC required) | Shorter (self-eval) |
| Pricing power | Higher (lock-in) | Lower (but offset by volume) |
| Churn | Lower (trapped) | Lower (voluntary loyalty) |
| Competitive moat | Code secrecy | Domain expertise + integrations |
| Acquisition multiple | Standard SaaS | Premium (community asset) |
| Talent attraction | Market rate | Premium (mission + visibility) |
| Customer references | Requires permission | Public usage visible |

### 4.4 The Moat Question

**"If it's open source, what's the moat?"**

The moat is not the code. The moat is:

1. **Domain judgment** — Knowing which HoS edge cases matter, which constraints dispatchers ignore, which "optimal" solutions drivers reject
2. **Integration expertise** — Nexus connectors require deep knowledge of each TMS/ELD API
3. **Operational excellence** — Running managed platform at scale with SLAs
4. **Velocity** — Shipping features faster than anyone could build internally
5. **Brand/trust** — Being the canonical OTTO, not a fork
6. **Relationships** — Direct lines to Girteka, Waberer's, other key accounts

Competitors can fork OTTO. They cannot fork the judgment layer, the relationships, or the velocity.

---

## 5. Value Proposition for Existing Nexogen Clients

### 5.1 Understanding Their Pain

Fleets using Nexogen's planning system (now Trimble) face:

| Pain Point | Severity |
|------------|----------|
| Uncertain roadmap | High — US-focused Trimble may deprioritize EU needs |
| Support quality | Medium — Team restructuring, contact churn |
| Pricing pressure | Medium — "Portfolio alignment" often means price increases |
| Integration maintenance | High — New TMS/ELD versions break connectors |
| Customization stagnation | High — Resources allocated to Trimble priorities |
| Exit cost | Very High — Years of integration work at risk |

### 5.2 OTTO's Value Proposition

**"Same capability, but you own it forever."**

| Nexogen Pain | OTTO Solution |
|--------------|---------------|
| Uncertain roadmap | Open source = you can fork if roadmap diverges |
| Support degradation | Dedicated support tier with SLAs, or self-support |
| Pricing increases | Self-host option caps your maximum cost |
| Integration breaks | Open integrations + Nexus connectors you can extend |
| No customization | Fork and customize, or pay for custom development |
| Lock-in anxiety | MIT license = permanent exit option |

### 5.3 Migration Path

**Phase 1: Parallel Operation (3-6 months)**
- Deploy OTTO alongside existing Nexogen
- Run shadow planning: compare outputs
- Validate domain logic matches expectations
- Zero disruption to operations

**Phase 2: Gradual Cutover (3-6 months)**
- Migrate non-critical routes to OTTO
- Build confidence with operations team
- Refine integrations (TMS, ELD, fuel cards)
- Train dispatchers on new interface

**Phase 3: Full Migration**
- Complete cutover to OTTO
- Decommission Nexogen
- Ongoing optimization and expansion

**Risk mitigation:** At any point, fleet can pause migration and continue with Nexogen. No cliff edge.

### 5.4 Pricing for Nexogen Migrants

**Incentive structure:**

| Offer | Purpose |
|-------|---------|
| Free parallel operation (6 months) | Remove evaluation risk |
| Migration services included | Reduce switching cost |
| Price match or beat Nexogen renewal | Remove budget objection |
| 2-year price lock | Counter "will you also raise prices?" |
| Source code escrow (if desired) | Additional assurance beyond open source |

**Message:** "You're already paying €400k/year for software you don't control. Pay us €350k/year for software you own forever."

### 5.5 Objection Handling

**"Why should we trust a new vendor?"**

"You shouldn't trust any vendor blindly. That's why the code is open source. If we fail, you have the code, the documentation, and the right to hire anyone to maintain it. Can Trimble say that?"

**"Open source means no support."**

"Open source means you have options. You can self-support, hire consultants, or pay us for enterprise SLAs. That's more support options than closed source, not fewer."

**"What if you get acquired too?"**

"The code stays MIT licensed forever. An acquisition can't change that. Worst case, you fork and continue. That's the point."

**"Our IT team can't maintain this."**

"Most clients run managed hosting. You don't maintain it; we do. Open source is your insurance policy, not your obligation."

**"We need features X, Y, Z that Nexogen has."**

"Show us. If it's core planning logic, we'll build it—open source. If it's integration, we'll add it to Nexus. Either way, you get it."

---

## 6. Go-To-Market Strategy

### 6.1 Phase 1: Foundation (Year 1)

**Objectives:**
- Ship all core engines (HoSE, Tempo, Arbor, Sigma, Pulse)
- Land 2-3 anchor enterprise customers
- Establish open source presence (GitHub, community)

**Tactics:**
- Direct outreach to known contacts (Girteka, Waberer's, others)
- Conference presence (transport logistic Munich, etc.)
- Technical content marketing (blog, benchmarks, case studies)
- GitHub presence (README, examples, documentation)

**Key metrics:**
- 3+ enterprise design partners
- 500+ GitHub stars
- €500k ARR

### 6.2 Phase 2: Growth (Years 2-3)

**Objectives:**
- Scale to 10-15 enterprise customers
- Expand Nexus connector library (10+ TMS/ELD integrations)
- Build mid-market motion (Professional tier)

**Tactics:**
- Case studies from anchor customers
- Partner channel (TMS vendors, consultants)
- Inbound from open source adoption
- Geographic expansion (US market entry)

**Key metrics:**
- 15+ enterprise customers
- 30+ professional tier customers
- €3-5M ARR
- <5% logo churn

### 6.3 Phase 3: Scale (Years 4-5)

**Objectives:**
- Market leadership in open planning systems
- Strategic acquisition target or sustainable independent growth
- Expand into adjacent markets (3PL, shipper)

**Tactics:**
- Enterprise sales team expansion
- Product-led growth for mid-market
- Strategic partnerships (ELD vendors, load boards)
- M&A of complementary tools

**Key metrics:**
- 50+ enterprise customers
- 100+ professional tier customers
- €8-15M ARR
- Acquisition interest or path to profitability

---

## 7. Competitive Positioning

### 7.1 Competitive Landscape

| Competitor | Positioning | OTTO Differentiation |
|------------|-------------|---------------------|
| Trimble/Nexogen | Incumbent, closed source | Open source, no lock-in |
| ORTEC | Enterprise, expensive | Faster, modern architecture |
| Descartes | Suite approach | Focused, best-of-breed planning |
| In-house builds | Control | Faster time-to-value, maintained |
| Generic solvers | Flexible | Domain-specific, ready-to-use |

### 7.2 Positioning Statement

**For** mid-to-large trucking fleets
**Who** need planning optimization without vendor lock-in
**OTTO is** an open-source logistics platform
**That** provides enterprise-grade route planning, HoS compliance, and fleet scheduling
**Unlike** closed-source vendors like Trimble
**OTTO** gives you full code ownership, transparent algorithms, and the freedom to self-host or migrate at any time.

---

## 8. Risk Analysis

### 8.1 Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Large competitor forks OTTO | Medium | Medium | Domain expertise moat, integration layer, relationship advantage |
| Slow enterprise sales | Medium | High | Open source inbound, mid-market tier for cash flow |
| Key person dependency | High | High | Documentation, hire early, spread knowledge |
| Open source community doesn't materialize | Medium | Low | Enterprise direct sales don't require community |
| Pricing pressure from free tier | Low | Medium | Clear tier separation, integration value |
| Technical debt / quality issues | Medium | High | Comprehensive test suite (279+ tests), CI/CD |

### 8.2 Key Success Factors

1. **Land 2-3 anchor customers in Year 1** — Proves model, generates case studies
2. **Ship complete planning stack** — HoSE, Tempo, Arbor, Sigma, Pulse
3. **Build Nexus connector library** — 10+ integrations for sales breadth
4. **Maintain engineering velocity** — AI-assisted development advantage
5. **Establish open source credibility** — GitHub presence, conference talks, content

---

## 9. Financial Summary

### 9.1 5-Year Projection

| Year | ARR | Customers | Headcount | Cash Position |
|------|-----|-----------|-----------|---------------|
| 1 | €500k | 5 | 3 | Bootstrapped / Seed |
| 2 | €1.5M | 15 | 6 | Cash flow positive |
| 3 | €3.5M | 30 | 12 | Series A optional |
| 4 | €6M | 50 | 20 | Profitable |
| 5 | €10M | 80 | 30 | Exit-ready |

### 9.2 Exit Scenarios

| Scenario | Timing | Valuation Range | Likely Buyers |
|----------|--------|-----------------|---------------|
| Early strategic | Year 2-3 | €15-30M | Sennder, Convoy, vertical integrators |
| Growth acquisition | Year 4-5 | €50-100M | Trimble, Samsara, Motive, Descartes |
| PE rollup | Year 5+ | €80-150M | Vista, Thoma Bravo |
| IPO / independence | Year 7+ | €200M+ | Public markets |

---

## 10. Conclusion

OTTO addresses a genuine market gap: enterprise fleets want planning optimization without vendor lock-in. The open-source model transforms the sales conversation from "trust us" to "here's the code."

The business model is proven (Red Hat, HashiCorp, GitLab). The market timing is right (post-Nexogen acquisition anxiety). The domain expertise exists (founding team built previous generation).

**The ask:** Build OTTO, land anchor customers, prove the model, and either scale independently or exit to a strategic buyer at a premium multiple.

**The thesis:** Open source + domain expertise + enterprise integrations = defensible, valuable business that either becomes a market leader or an attractive acquisition target.

---

*Document version: 1.0*
*Last updated: February 2026*
