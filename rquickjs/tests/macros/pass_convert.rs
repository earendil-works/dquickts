use rquickjs::{class::Trace, CatchResultExt, Context, FromJs, IntoJs, JsLifetime, Runtime};

#[derive(Debug, PartialEq, Eq, FromJs, IntoJs)]
#[qjs(rename_all = "camelCase")]
struct RenamedFields {
    some_value: u32,
    #[qjs(rename = "labelText")]
    label_text: String,
}

#[derive(Debug, PartialEq, Eq, FromJs, IntoJs)]
struct Pair(u32, String);

/// Set as by `Object::set`: `__proto__` sets the prototype (ignored for a number)
#[derive(IntoJs)]
struct WithProto {
    a: u32,
    #[qjs(rename = "__proto__")]
    p: u32,
    b: u32,
}

#[derive(Clone, Debug, PartialEq, Eq, Trace, JsLifetime)]
#[rquickjs::class]
struct JsClassShape {
    value: u32,
}

pub fn main() {
    let rt = Runtime::new().unwrap();
    let ctx = Context::full(&rt).unwrap();

    ctx.with(|ctx| {
        ctx.globals()
            .set(
                "renamed",
                RenamedFields {
                    some_value: 1,
                    label_text: "alpha".into(),
                },
            )
            .unwrap();

        ctx.eval::<(), _>(
            r#"
            if (renamed.someValue !== 1) {
                throw new Error("someValue");
            }
            if (renamed.labelText !== "alpha") {
                throw new Error("labelText");
            }
        "#,
        )
        .catch(&ctx)
        .unwrap();

        let renamed: RenamedFields = ctx
            .eval(r#"({ someValue: 2, labelText: "beta" })"#)
            .unwrap();
        assert_eq!(
            renamed,
            RenamedFields {
                some_value: 2,
                label_text: "beta".into(),
            }
        );

        for _ in 0..2 {
            ctx.globals()
                .set("withProto", WithProto { a: 1, p: 2, b: 3 })
                .unwrap();
            ctx.eval::<(), _>(
                r#"
                if (Object.keys(withProto).join() !== "a,b" || withProto.b !== 3) {
                    throw new Error("withProto");
                }
                if (Object.getPrototypeOf(withProto) !== Object.prototype) {
                    throw new Error("withProto prototype");
                }
            "#,
            )
            .catch(&ctx)
            .unwrap();
        }

        ctx.globals().set("pair", Pair(3, "gamma".into())).unwrap();

        ctx.eval::<(), _>(
            r#"
            if (pair[0] !== 3) {
                throw new Error("pair0");
            }
            if (pair[1] !== "gamma") {
                throw new Error("pair1");
            }
        "#,
        )
        .catch(&ctx)
        .unwrap();

        let pair: Pair = ctx.eval(r#"[4, "delta"]"#).unwrap();
        assert_eq!(pair, Pair(4, "delta".into()));

        ctx.globals()
            .set("classValue", JsClassShape { value: 7 })
            .unwrap();

        let class_value: JsClassShape = ctx.eval("classValue").unwrap();
        assert_eq!(class_value.value, 7);

        let class_from_literal = ctx.eval::<JsClassShape, _>(r#"({ value: 8 })"#);
        assert!(class_from_literal.is_err());
    });
}
