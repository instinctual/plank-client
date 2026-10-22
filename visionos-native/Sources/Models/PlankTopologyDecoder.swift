import Foundation

/// Authenticated v13 geometry. Rendering and input keep this exact snapshot;
/// rectangles are never inferred from names, connector order or local screens.
enum PlankTopologyDecoder {
    struct Invalid: LocalizedError {
        var errorDescription: String? { "The Host returned malformed display information." }
    }
    static func decode(_ data: Data) throws -> PlankTopology {
        guard data.count <= 1_048_576,
              let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let version = object["schema_version"] as? Int, version == 13,
              let flags = object["feature_flags"] as? Int, flags >= 0,
              let generation = object["generation"] as? String, !generation.isEmpty,
              let desktop = object["desktop"] as? [String: Any],
              let width = desktop["width"] as? Int, let height = desktop["height"] as? Int,
              width > 0, width <= 16384, height > 0, height <= 16384,
              let layout = object["layout"] as? [String: Any],
              let kind = layout["kind"] as? String,
              let modes = layout["virtual_modes"] as? [String] else { throw Invalid() }
        var outputs: [PlankTopology.Output] = []
        if let records = object["outputs"] as? [[String: Any]] {
            guard !records.isEmpty, records.count <= 16 else { throw Invalid() }
            for output in records {
                guard let id = output["id"] as? String, !id.isEmpty,
                      !outputs.contains(where: { $0.id == id }),
                      let name = output["name"] as? String,
                      let x = output["x"] as? Int, let y = output["y"] as? Int,
                      let w = output["width"] as? Int, let h = output["height"] as? Int,
                      let primary = output["primary"] as? Bool,
                      let source = output["source_rect"] as? [String: Any],
                      let sx = source["x"] as? Int, let sy = source["y"] as? Int,
                      let sw = source["width"] as? Int, let sh = source["height"] as? Int,
                      w > 0, h > 0, sw == w, sh == h,
                      abs(Double(x)) <= 1_000_000, abs(Double(y)) <= 1_000_000 else { throw Invalid() }
                let rect = PlankTopology.Rect(x: sx, y: sy, width: sw, height: sh)
                guard rect.fits(width: width, height: height) else { throw Invalid() }
                outputs.append(.init(id: id, name: name, x: x, y: y, width: w, height: h,
                                     primary: primary, sourceRect: rect))
            }
        } else if flags & 0x40 != 0 { throw Invalid() }
        if kind == "dual-horizontal" {
            guard flags & 0x140 == 0x140, outputs.count == 2, modes.count == 2 else { throw Invalid() }
            let ordered = outputs.sorted { ($0.x, $0.y, $0.id) < ($1.x, $1.y, $1.id) }
            guard ordered[0].sourceRect.x + ordered[0].width == ordered[1].sourceRect.x,
                  ordered[0].sourceRect.y == ordered[1].sourceRect.y,
                  ordered[0].sourceRect.x == 0, ordered[0].sourceRect.y == 0,
                  ordered[1].sourceRect.x + ordered[1].width == width,
                  max(ordered[0].height, ordered[1].height) == height,
                  outputs.filter({ $0.primary }).count == 1 else { throw Invalid() }
        }
        return PlankTopology(schemaVersion: version, featureFlags: flags, generation: generation,
            desktopWidth: width, desktopHeight: height,
            layout: .init(kind: kind, virtualModes: modes, startupKind: layout["startup_kind"] as? String),
            desktopX: desktop["x"] as? Int ?? 0, desktopY: desktop["y"] as? Int ?? 0, outputs: outputs)
    }
}
