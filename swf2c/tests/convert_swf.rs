//! End-to-end: a SWF written with the `swf` crate goes through parse, convert and
//! serialise, and the resulting assets.bin has the expected records.

use swf::*;
use swf2c::assets_bin::{self, find_block, kind};
use swf2c::{convert, parse};

fn rect(w: f64, h: f64) -> Rectangle<Twips> {
    Rectangle {
        x_min: Twips::ZERO,
        y_min: Twips::ZERO,
        x_max: Twips::from_pixels(w),
        y_max: Twips::from_pixels(h),
    }
}

fn square_records() -> Vec<ShapeRecord> {
    let mut v = vec![ShapeRecord::StyleChange(Box::new(StyleChangeData {
        move_to: Some(Point::new(Twips::ZERO, Twips::ZERO)),
        fill_style_0: None,
        fill_style_1: Some(1),
        line_style: None,
        new_styles: None,
    }))];
    for (dx, dy) in [(400, 0), (0, 400), (-400, 0), (0, -400)] {
        v.push(ShapeRecord::StraightEdge {
            delta: PointDelta::new(Twips::new(dx), Twips::new(dy)),
        });
    }
    v
}

fn place(id: u16, depth: u16) -> Tag<'static> {
    Tag::PlaceObject(Box::new(PlaceObject {
        version: 2,
        action: PlaceObjectAction::Place(id),
        depth,
        matrix: Some(Matrix::translate(Twips::from_pixels(10.0), Twips::ZERO)),
        color_transform: None,
        ratio: None,
        name: Some(SwfStr::from_utf8_str("thing")),
        clip_depth: None,
        class_name: None,
        filters: None,
        background_color: None,
        blend_mode: None,
        clip_actions: None,
        has_image: false,
        is_bitmap_cached: None,
        is_visible: None,
        amf_data: None,
    }))
}

fn build_swf() -> Vec<u8> {
    // trace("hi") as AVM1: Push "hi"; Trace; End.
    let action: &'static [u8] = &[0x96, 0x04, 0x00, 0x00, b'h', b'i', 0x00, 0x26, 0x00];
    let pcm: &'static [u8] = Box::leak(
        (0..2000)
            .flat_map(|i: i32| (((i % 100) - 50) as i16 * 300).to_le_bytes())
            .collect::<Vec<u8>>()
            .into_boxed_slice(),
    );
    let tags = vec![
        Tag::DefineSound(Sound {
            id: 3,
            format: SoundFormat {
                compression: AudioCompression::Uncompressed,
                sample_rate: 11025,
                is_stereo: false,
                is_16_bit: true,
            },
            num_samples: 2000,
            data: pcm,
        }),
        Tag::DefineShape(Box::new(Shape {
            version: 1,
            id: 1,
            shape_bounds: rect(20.0, 20.0),
            edge_bounds: rect(20.0, 20.0),
            flags: ShapeFlag::empty(),
            styles: ShapeStyles {
                fill_styles: vec![FillStyle::Color(Color::from_rgb(0x336699, 255))],
                line_styles: vec![],
            },
            shape: square_records(),
        })),
        Tag::DefineSprite(Sprite {
            id: 2,
            num_frames: 2,
            tags: vec![place(1, 1), Tag::ShowFrame, Tag::ShowFrame],
        }),
        place(2, 1),
        Tag::DoAction(action),
        Tag::FrameLabel(FrameLabel {
            label: SwfStr::from_utf8_str("start"),
            is_anchor: false,
        }),
        Tag::ShowFrame,
    ];
    let header = Header {
        compression: Compression::Zlib,
        version: 8,
        stage_size: rect(550.0, 400.0),
        frame_rate: Fixed8::from_f32(24.0),
        num_frames: 1,
    };
    let mut out = Vec::new();
    swf::write_swf(&header, &tags, &mut out).unwrap();
    out
}

#[test]
fn converts_a_small_movie() {
    let bytes = build_swf();
    let buf = parse::decompress(&bytes).unwrap();
    let movie = parse::parse(&buf).unwrap();
    let out = convert::convert(&movie, &convert::Options::default()).unwrap();
    assert!(out.warnings.is_empty(), "{:?}", out.warnings);
    let a = &out.assets;
    assert!((a.target_scale - 0.68).abs() < 1e-3);
    assert_eq!(a.frame_rate, 24 << 8);
    assert_eq!(a.chars[1].kind, kind::SHAPE);
    assert_eq!(a.chars[2].kind, kind::SPRITE);
    assert_eq!(a.sprites.len(), 2);
    assert_eq!(a.sprites[a.chars[2].index as usize].frame_count, 2);
    assert_eq!(a.scripts.len(), 1);
    assert_eq!(a.labels.len(), 1);
    assert_eq!(a.draws.len(), 1);
    assert_eq!(a.draws[0].idx_count, 6);
    assert_eq!(a.chars[3].kind, kind::SOUND);
    let snd = &a.sounds[a.chars[3].index as usize];
    assert_eq!(
        (snd.format, snd.channels, snd.rate, snd.frames),
        (assets_bin::sound_format::IMA4, 1, 11025, 2000)
    );
    assert_eq!(snd.data_size, 2 * (4 + 512));

    let bin = assets_bin::serialize(a);
    let (cmds, _) = find_block(&bin, b"CMDS").unwrap();
    // The root frame: PLACE char 2 at depth 1, then ACTION script 0.
    let root_frames = &a.frames[a.sprites[0].frame_table as usize..];
    let f0 = &cmds[root_frames[0] as usize..root_frames[1] as usize];
    assert_eq!(f0[0], assets_bin::cmd::PLACE);
    assert_eq!(&f0[f0.len() - 5..], &[assets_bin::cmd::ACTION, 0, 0, 0, 0]);
    let (strs, _) = find_block(&bin, b"STRS").unwrap();
    assert!(strs.windows(6).any(|w| w == b"thing\0"));
    assert!(strs.windows(6).any(|w| w == b"start\0"));
}

#[test]
fn rejects_avm2_movies() {
    let header = Header {
        compression: Compression::None,
        version: 10,
        stage_size: rect(10.0, 10.0),
        frame_rate: Fixed8::ONE,
        num_frames: 1,
    };
    let tags = [
        Tag::FileAttributes(FileAttributes::IS_ACTION_SCRIPT_3),
        Tag::ShowFrame,
    ];
    let mut out = Vec::new();
    swf::write_swf(&header, &tags, &mut out).unwrap();
    let buf = parse::decompress(&out).unwrap();
    let err = parse::parse(&buf).expect_err("AVM2 must be rejected");
    assert!(err.to_string().contains("ActionScript 3"));
}
