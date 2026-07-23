# CappedWeather — TODO

## Active

- [ ] **Lightning-jump engine (GLM L2 + Level II fusion)** — open-source real-time lightning jump detection (2σ flash-rate surge algorithm) on GOES GLM data, fused with NEXRAD. No open real-time implementation exists; NOAA-internal only. Spec: `docs/lightning-jump-engine.md`. **IN PROGRESS**

## Backlog — saved research ideas

- [ ] **GPU volume rendering of live Level II** — ray-marched 3D dual-pol volumes at interactive framerates, open source. GR2Analyst does 3D closed/paid/clunky; the open world has nothing good. Novel as a thing that exists, pure GPU pain.
- [ ] **Storm-scale severe nowcasting** — 0–60 min nowcast of mesocyclone tracks / azimuthal shear on super-res radar (budget warn-on-forecast). NOT precipitation nowcasting (Google MetNet owns that). Highest impact, highest risk — needs ML training infra + labeled storm archives.
- [ ] **Open-data VLF lightning geolocation network** — TDoA lightning locating on the existing global KiwiSDR fleet + ML signal ID. Open-data alternative to closed Blitzortung. Slow burn: hardware/community dependent.
