/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_collage.h"
#include "oblivion/oblivion_photo_doc.h"
#include "oblivion/oblivion_photo_draw.h"
#include "oblivion/oblivion_photo_fx.h"
#include "oblivion/oblivion_photo_transform.h"

#include <QtCore/QElapsedTimer>

#include <thread>

// Photo editor: the checks of all the parts working together, the "all"
// sub-test of OBLIVION_SELFTEST=photo_doc.
//
// The modules test themselves (the "adjust", "blur", "draw", "collage"...
// sub-tests). Here one document has layers of every kind that can be made
// without the interface (photos, a drawing, a collage) with a mask,
// transforms, blend modes and effects of every group on them, and what
// the editor does with a document is done with it: the proxy preview
// against the full-resolution export, the caches against a clean render,
// several renders at once, undo and redo through edits of every kind,
// merging, flattening, taking the collage apart, another canvas size and
// the serialized forms.
//
// Pure: no interface, no lang strings, no session, deterministic.
namespace Oblivion::Photo {
namespace {

struct Sample {
	Document document;
	LayerId photo = 0;
	LayerId card = 0;
	LayerId drawing = 0;
	LayerId collage = 0;
	std::vector<QByteArray> missing; // Effects that are not registered.
};

[[nodiscard]] DrawShape Stroke(
		DrawKind kind,
		QColor color,
		double width,
		std::vector<DrawPoint> points) {
	auto result = DrawShape();
	result.kind = kind;
	result.color = color;
	result.width = width;
	result.points = std::move(points);
	return result;
}

[[nodiscard]] MaskPtr GradientMask(QSize content) {
	const auto size = MaskSizeFor(content);
	auto image = QImage(size, QImage::Format_Grayscale8);
	if (image.isNull()) {
		return nullptr;
	}
	for (auto y = 0; y != size.height(); ++y) {
		const auto line = image.scanLine(y);
		for (auto x = 0; x != size.width(); ++x) {
			line[x] = uchar(40 + 215 * x / std::max(size.width() - 1, 1));
		}
	}
	return MakeMask(std::move(image));
}

[[nodiscard]] Sample MakeSample() {
	auto result = Sample();
	const auto use = [&](
			const QByteArray &id,
			std::initializer_list<FxParams::Entry> values = {}) {
		if (!FindFx(id)) {
			result.missing.push_back(id);
		}
		return MakeFx(id, values);
	};

	auto &document = result.document;
	document = DocumentFromImage(FxTestImage(640, 480), EditState(), u"photo"_q);
	result.photo = document.layers.front().id;
	{
		auto &photo = document.layers.front();
		photo.effects.push_back(use("adjust.light", {
			{ "exposure", FxValue::Number(0.4) },
			{ "contrast", FxValue::Integer(20) },
		}));
		photo.effects.push_back(use("adjust.hsl", {
			{ "table", FxValue::Data("h=0,0,20,0,0,0,0,0/s=0,0,0,30") },
		}));
		photo.effects.push_back(use("blur.gaussian", {
			{ "radius", FxValue::Number(9.) },
			{ "region", FxValue::Integer(2) },
		}));
		for (auto &instance : photo.effects) {
			instance.uid = document.nextId++;
		}
	}

	auto card = MakeImageLayer(FxTestImage(300, 200, true), u"card"_q);
	card.transform = ComposeTransform({
		.center = QPointF(420., 170.),
		.scaleX = 0.9,
		.scaleY = -0.9,
		.rotation = 14.,
	}, QSizeF(300., 200.));
	card.blend = BlendMode::Overlay;
	card.opacity = 0.7;
	card.mask = GradientMask(card.size());
	card.effects.push_back(use("distort.wave", {
		{ "amplitude", FxValue::Number(8.) },
		{ "wavelength", FxValue::Number(60.) },
	}));
	card.effects.push_back(use("glitch.rgb", {
		{ "red_x", FxValue::Number(6.) },
		{ "blue_y", FxValue::Number(-4.) },
	}));
	card.effects.push_back(use("lofi.pixelate", {
		{ "size", FxValue::Number(6.) },
	}));
	result.card = AddLayer(document, std::move(card));

	auto shapes = std::vector<DrawShape>();
	shapes.push_back(Stroke(DrawKind::Pen, QColor(230, 40, 60), 14., {
		{ 60.f, 400.f, 1.f },
		{ 160.f, 300.f, 0.8f },
		{ 260.f, 380.f, 1.f },
		{ 380.f, 320.f, 0.6f },
	}));
	shapes.push_back(Stroke(DrawKind::Arrow, QColor(20, 90, 220), 9., {
		{ 520.f, 420.f },
		{ 330.f, 250.f },
	}));
	shapes.push_back(Stroke(DrawKind::Rectangle, QColor(250, 210, 40), 6., {
		{ 40.f, 40.f },
		{ 220.f, 150.f },
	}));
	shapes.back().filled = true;
	shapes.back().opacity = 0.6;
	shapes.push_back(Stroke(DrawKind::Eraser, QColor(0, 0, 0), 26., {
		{ 30.f, 100.f, 1.f },
		{ 240.f, 90.f, 1.f },
	}));
	auto drawing = MakeLayer(
		MakeDrawContent(document.size, std::move(shapes)),
		u"drawing"_q);
	drawing.blend = BlendMode::Multiply;
	drawing.effects.push_back(use("blur.box", {
		{ "radius", FxValue::Number(2.) },
	}));
	result.drawing = AddLayer(document, std::move(drawing));

	auto images = std::vector<ImportedImage>();
	images.push_back({ FxTestImage(320, 240), u"one"_q });
	images.push_back({ FxTestImage(200, 300, true), u"two"_q });
	images.push_back({ FxTestImage(260, 260), u"three"_q });
	auto made = CollageDocument(
		PrepareCollagePhotos(std::move(images)),
		u"collage"_q,
		QSize(360, 270));
	if (!made.layers.empty()) {
		auto collage = made.layers.front();
		if (const auto content = AsCollage(collage.content)) {
			auto data = content->data();
			data.spacing = 0.03;
			data.margin = 0.04;
			data.radius = 0.08;
			data.background = CollageBackground::Gradient;
			data.color1 = QColor(250, 200, 150);
			data.color2 = QColor(90, 40, 140);
			collage.content = MakeCollageContent(std::move(data));
		}
		collage.effects.clear();
		collage.transform = ComposeTransform({
			.center = QPointF(170., 330.),
			.scaleX = 0.6,
			.scaleY = 0.6,
			.rotation = -6.,
		}, QSizeF(collage.size()));
		collage.opacity = 0.9;
		for (const auto preset : AllFxPresets()) {
			if (preset->id == "lofi.preset_phone") {
				collage.effects = preset->stack;
			}
		}
		result.collage = AddLayer(document, std::move(collage));
	}

	document.global.crop = QRectF(0.04, 0.06, 0.9, 0.88);
	document.global.quarterTurns = 1;
	document.global.contrast = 12;
	document.global.vignette = 20;
	return result;
}

[[nodiscard]] bool SameImage(const QImage &a, const QImage &b) {
	return !a.isNull()
		&& (a.size() == b.size())
		&& (FxImageDifference(a, b) == 0.);
}

bool RunAllSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	auto timer = QElapsedTimer();
	timer.start();

	const auto sample = MakeSample();
	const auto &document = sample.document;
	if (!sample.missing.empty()) {
		// A build without some effect module (a test harness): those
		// instances do nothing, everything else is still checked.
		auto list = QStringList();
		for (const auto &id : sample.missing) {
			list.push_back(QString::fromLatin1(id));
		}
		info(u"not registered here: %1"_q.arg(list.join(u", "_q)));
	}
	check(
		document.layers.size() >= 3
			&& sample.photo
			&& sample.card
			&& sample.drawing
			&& IsDrawContent(document.find(sample.drawing)->content)
			&& !IsPlainImage(document),
		u"all: a document with photos, a drawing%1, a mask and effects"_q.arg(
			sample.collage ? u", a collage"_q : QString()));

	// The export and the proxy preview.
	const auto exported = RenderDocument(document);
	check(
		!exported.isNull()
			&& (exported.size() == OutputSize(document))
			&& (exported.format() == QImage::Format_ARGB32_Premultiplied)
			&& SameImage(exported, RenderDocument(document)),
		u"all: the export has the output size and is repeatable"_q);
	{
		auto compositor = Compositor();
		const auto half = compositor.render(document, {
			.scale = 0.5,
			.preview = true,
			.active = sample.card,
		});
		const auto third = compositor.render(document, {
			.scale = 1. / 3.,
			.preview = true,
		});
		const auto halfDifference = half.isNull()
			? 255.
			: FxImageDifference(half, exported);
		const auto thirdDifference = third.isNull()
			? 255.
			: FxImageDifference(third, exported);
		check(
			!half.isNull()
				&& !third.isNull()
				&& (half.width() < exported.width())
				&& (halfDifference < 10.)
				&& (thirdDifference < 12.),
			u"all: the proxy preview looks like the export"_q);
		info(u"preview against export: %1 at 1/2, %2 at 1/3 (of 255)"_q.arg(
			QString::number(halfDifference, 'f', 2),
			QString::number(thirdDifference, 'f', 2)));

		// The caches give what a clean render gives.
		const auto cold = compositor.render(document, { .scale = 1. });
		const auto warm = compositor.render(document, {
			.scale = 1.,
			.active = sample.drawing,
		});
		check(
			SameImage(cold, exported) && SameImage(warm, exported),
			u"all: cached renders are the export, pixel for pixel"_q);

		// A change of one layer over warm caches.
		auto changed = document;
		changed.find(sample.card)->opacity = 0.35;
		LayerFx(
			changed,
			sample.photo,
			changed.find(sample.photo)->effects.front().uid
		)->params.set("contrast", FxValue::Integer(-30));
		check(
			SameImage(
				compositor.render(changed, {
					.scale = 1.,
					.active = sample.card,
				}),
				RenderDocument(changed)),
			u"all: an edit over warm caches renders like a clean one"_q);

		// Several renders at once (the previews and the thumbnails do).
		auto results = std::array<QImage, 4>();
		auto threads = std::vector<std::thread>();
		for (auto i = 0; i != int(results.size()); ++i) {
			threads.emplace_back([&, i] {
				results[i] = compositor.render(document, {
					.scale = (i % 2) ? 0.5 : 0.25,
					.preview = true,
				});
			});
		}
		for (auto &thread : threads) {
			thread.join();
		}
		auto fresh = Compositor();
		check(
			SameImage(results[1], results[3])
				&& SameImage(results[0], results[2])
				&& SameImage(results[1], fresh.render(document, {
					.scale = 0.5,
					.preview = true,
				})),
			u"all: renders running at once give the same pictures"_q);

		auto stop = std::atomic<bool>(true);
		check(
			RenderDocument(document, QSize(), &stop).isNull()
				&& compositor.render(document, {
					.scale = 0.5,
					.cancel = &stop,
				}).isNull(),
			u"all: a cancelled render gives nothing"_q);
	}

	// Undo and redo through edits of every kind.
	{
		auto steps = std::vector<Document>();
		steps.push_back(document);
		const auto next = [&](Fn<void(Document&)> modify) {
			auto copy = steps.back();
			modify(copy);
			steps.push_back(std::move(copy));
		};
		next([&](Document &d) {
			d.global.contrast = -5;
			d.global.crop = QRectF(0., 0., 1., 1.);
		});
		next([&](Document &d) {
			d.find(sample.card)->blend = BlendMode::Difference;
			d.find(sample.card)->transform = FlippedTransform(
				d.find(sample.card)->transform,
				QSizeF(d.find(sample.card)->size()),
				true);
		});
		next([&](Document &d) {
			AddLayerFx(d, sample.drawing, MakeFx("classic.invert"));
			MoveLayer(d, sample.drawing, 0);
		});
		next([&](Document &d) {
			const auto content = d.find(sample.drawing)->content;
			d.find(sample.drawing)->content = DrawWithShape(
				content,
				Stroke(DrawKind::Marker, QColor(40, 200, 90), 30., {
					{ 100.f, 200.f, 1.f },
					{ 500.f, 220.f, 1.f },
				}));
		});
		next([&](Document &d) {
			d.find(sample.card)->mask = MakeMask(
				InvertedMask(d.find(sample.card)->mask->image()));
			d.find(sample.photo)->visible = false;
		});
		next([&](Document &d) {
			d = CanvasResized(d, QSize(800, 800));
		});
		next([&](Document &d) {
			DuplicateLayer(d, sample.card);
			RemoveLayer(d, sample.photo);
		});

		auto history = History(document);
		auto pushed = true;
		for (auto i = 1; i != int(steps.size()); ++i) {
			pushed = history.push(steps[i]) && pushed;
		}
		const auto last = RenderDocument(steps.back());
		auto back = true;
		for (auto i = int(steps.size()) - 1; i != 0; --i) {
			back = history.undo() && (history.current() == steps[i - 1]) && back;
		}
		const auto restored = SameImage(
			RenderDocument(history.current()),
			exported);
		auto forward = true;
		for (auto i = 1; i != int(steps.size()); ++i) {
			forward = history.redo() && (history.current() == steps[i]) && forward;
		}
		check(
			pushed
				&& back
				&& restored
				&& forward
				&& !history.canRedo()
				&& !last.isNull()
				&& SameImage(RenderDocument(history.current()), last),
			u"all: %1 edits of every kind undo and redo exactly"_q.arg(
				int(steps.size()) - 1));
	}

	// Operations that make pixels keep the picture.
	const auto flat = [](const Document &document) {
		auto copy = document;
		copy.global = EditState();
		return RenderDocument(copy);
	};
	const auto before = flat(document);
	{
		const auto merged = MergedDown(document, sample.card);
		const auto difference = merged
			? FxImageDifference(before, flat(*merged))
			: 255.;
		check(
			merged
				&& (merged->layers.size() + 1 == document.layers.size())
				&& !merged->find(sample.card)
				&& merged->find(sample.photo)
				&& (difference < 1.),
			u"all: merging down keeps the picture (differs by %1)"_q.arg(
				QString::number(difference, 'f', 3)));
	}
	{
		const auto flattened = Flattened(document);
		const auto difference = flattened
			? FxImageDifference(before, flat(*flattened))
			: 255.;
		check(
			flattened
				&& (flattened->layers.size() == 1)
				&& (flattened->size == document.size)
				&& (flattened->global == document.global)
				&& (difference < 1.),
			u"all: flattening keeps the picture (differs by %1)"_q.arg(
				QString::number(difference, 'f', 3)));
	}
	if (sample.collage) {
		// The collage here has effects, they go to every layer it becomes:
		// only without them the two pictures must be the same.
		// The same for its opacity: one sheet seen through is not several
		// sheets seen through one over another.
		auto plain = document;
		plain.find(sample.collage)->effects.clear();
		plain.find(sample.collage)->opacity = 1.;
		const auto apart = CollageToLayers(
			plain,
			sample.collage,
			u"background"_q,
			u"cell %1"_q);
		const auto difference = apart
			? FxImageDifference(flat(plain), flat(*apart))
			: 255.;
		check(
			apart
				&& (apart->layers.size() > plain.layers.size())
				&& !apart->find(sample.collage)
				&& (difference < 1.5),
			u"all: a collage taken apart keeps the picture (differs by %1)"_q
				.arg(QString::number(difference, 'f', 3)));
	}
	{
		const auto larger = CanvasResized(document, QSize(900, 700));
		const auto after = flat(larger);
		const auto shift = QPoint((900 - 640) / 2, (700 - 480) / 2);
		check(
			!after.isNull()
				&& (after.size() == QSize(900, 700))
				&& (FxImageDifference(
					before,
					after.copy(QRect(shift, document.size))) < 0.5)
				&& (OutputSize(larger) == QSize(700, 900)),
			u"all: a larger canvas keeps every layer where it was"_q);
	}

	// What is stored comes back the same.
	{
		auto same = true;
		for (const auto &layer : document.layers) {
			const auto restored = DeserializeFxStack(
				SerializeFxStack(layer.effects));
			same = same && (restored == layer.effects);
		}
		const auto drawing = document.find(sample.drawing)->content;
		const auto restored = DeserializeDrawing(SerializeDrawing(drawing));
		const auto request = ContentRequest{ .scale = 0.5 };
		check(
			same
				&& restored
				&& (DrawShapes(restored) == DrawShapes(drawing))
				&& SameImage(
					restored->render(request),
					drawing->render(request)),
			u"all: effect stacks and the drawing survive serialization"_q);
	}
	info(u"all: %1 ms"_q.arg(timer.elapsed()));
	return ok;
}

const auto AllSelfTest = SelfTestRegistrar(
	SelfTestSuite::Doc,
	"all",
	&RunAllSelfTest);

} // namespace
} // namespace Oblivion::Photo
