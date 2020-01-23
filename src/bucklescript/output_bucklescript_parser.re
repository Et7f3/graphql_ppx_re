open Graphql_ppx_base;
open Result_structure;
open Schema;

open Ast_406;
open Asttypes;
open Parsetree;

open Generator_utils;
open Output_bucklescript_utils;

let const_str_expr = s => Ast_helper.(Exp.constant(Pconst_string(s, None)));

let lean_parse = () => Ppx_config.lean_parse();

let make_error_raiser = message =>
  if (Ppx_config.verbose_error_handling()) {
    %expr
    Js.Exn.raiseError("graphql_ppx: " ++ [%e message]);
  } else {
    %expr
    Js.Exn.raiseError("Unexpected GraphQL query response");
  };

let string_decoder = loc =>
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeString(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr "Expected string, got " ++ Js.Json.stringify(value)],
      )
    | Some(value) => (value: string)
    }
  );
let float_decoder = loc =>
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeNumber(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr "Expected float, got " ++ Js.Json.stringify(value)],
      )
    | Some(value) => value
    }
  );

let int_decoder = loc =>
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeNumber(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr "Expected int, got " ++ Js.Json.stringify(value)],
      )
    | Some(value) => int_of_float(value)
    }
  );

let boolean_decoder = loc =>
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeBoolean(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr "Expected boolean, got " ++ Js.Json.stringify(value)],
      )
    | Some(value) => value
    }
  );
let id_decoder = string_decoder;

let string_decoder_lean = loc =>
  [@metaloc loc] [%expr (Obj.magic(value): string)];
let id_decoder_lean = string_decoder_lean;
let float_decoder_lean = loc =>
  [@metaloc loc] [%expr (Obj.magic(value): float)];
let int_decoder_lean = loc => [@metaloc loc] [%expr (Obj.magic(value): int)];
let boolean_decoder_lean = loc =>
  [@metaloc loc] [%expr (Obj.magic(value): bool)];

let generate_poly_enum_decoder = (loc, enum_meta) => {
  let enum_match_arms =
    Ast_helper.(
      enum_meta.em_values
      |> List.map(({evm_name, _}) =>
           Exp.case(
             Pat.constant(Pconst_string(evm_name, None)),
             Exp.variant(evm_name, None),
           )
         )
    );
  let fallback_arm =
    Ast_helper.(
      Exp.case(
        Pat.any(),
        make_error_raiser(
          [%expr
            "Unknown enum variant for "
            ++ [%e const_str_expr(enum_meta.em_name)]
            ++ ": "
            ++ value
          ],
        ),
      )
    );
  let match_expr =
    Ast_helper.(
      Exp.match(
        [%expr value],
        List.concat([enum_match_arms, [fallback_arm]]),
      )
    );

  let enum_ty =
    [@metaloc loc]
    Ast_helper.(
      Typ.variant(
        enum_meta.em_values
        |> List.map(({evm_name, _}) =>
             Rtag({txt: evm_name, loc}, [], true, [])
           ),
        Closed,
        None,
      )
    );

  switch%expr (Js.Json.decodeString(value)) {
  | None =>
    %e
    make_error_raiser(
      [%expr
        "Expected enum value for "
        ++ [%e const_str_expr(enum_meta.em_name)]
        ++ ", got "
        ++ Js.Json.stringify(value)
      ],
    )
  | Some(value) => ([%e match_expr]: [%t enum_ty])
  };
};

let generate_solo_fragment_spread = (loc, name) => {
  let ident =
    Ast_helper.Exp.ident({loc, txt: Longident.parse(name ++ ".parse")});
  %expr
  [%e ident](value);
};

let generate_error = (loc, message) => {
  let ext = Ast_mapper.extension_of_error(Location.error(~loc, message));
  let%expr _value = value;
  %e
  Ast_helper.Exp.extension(~loc, ext);
};

let rec generate_parser = config =>
  fun
  | Res_nullable(loc, inner) =>
    lean_parse()
      ? generate_nullable_decoder_lean(config, conv_loc(loc), inner)
      : generate_nullable_decoder(config, conv_loc(loc), inner)
  | Res_array(loc, inner) =>
    lean_parse()
      ? generate_array_decoder_lean(config, conv_loc(loc), inner)
      : generate_array_decoder(config, conv_loc(loc), inner)
  | Res_id(loc) =>
    lean_parse()
      ? id_decoder_lean(conv_loc(loc)) : id_decoder(conv_loc(loc))
  | Res_string(loc) =>
    lean_parse()
      ? string_decoder_lean(conv_loc(loc)) : string_decoder(conv_loc(loc))
  | Res_int(loc) =>
    lean_parse()
      ? int_decoder_lean(conv_loc(loc)) : int_decoder(conv_loc(loc))
  | Res_float(loc) =>
    lean_parse()
      ? float_decoder_lean(conv_loc(loc)) : float_decoder(conv_loc(loc))
  | Res_boolean(loc) =>
    lean_parse()
      ? boolean_decoder_lean(conv_loc(loc))
      : boolean_decoder(conv_loc(loc))
  | Res_raw_scalar(_) => [%expr value]
  | Res_poly_enum(loc, enum_meta) =>
    generate_poly_enum_decoder(conv_loc(loc), enum_meta)
  | Res_custom_decoder(loc, ident, inner) =>
    generate_custom_decoder(config, conv_loc(loc), ident, inner)
  | Res_record(loc, name, fields) =>
    generate_object_decoder(config, conv_loc(loc), name, fields)
  | Res_object(loc, name, fields) =>
    generate_object_decoder(config, conv_loc(loc), name, fields)
  | Res_poly_variant_union(loc, name, fragments, exhaustive) =>
    generate_poly_variant_union(
      config,
      conv_loc(loc),
      name,
      fragments,
      exhaustive,
    )
  | Res_poly_variant_interface(loc, name, base, fragments) =>
    generate_poly_variant_interface(
      config,
      conv_loc(loc),
      name,
      base,
      fragments,
    )
  | Res_solo_fragment_spread(loc, name) =>
    generate_solo_fragment_spread(conv_loc(loc), name)
  | Res_error(loc, message) => generate_error(conv_loc(loc), message)
and generate_nullable_decoder_lean = (config, loc, inner) =>
  [@metaloc loc]
  (
    switch%expr (Js.toOption(Obj.magic(value): Js.Nullable.t('a))) {
    | Some(_) => Some([%e generate_parser(config, inner)])
    | None => None
    }
    // (Obj.magic(value): Js.Nullable.t('a)) == Js.Nullable.null
    // || (Obj.magic(value): Js.Nullable.t('a)) == Js.Nullable.undefined
  )
and generate_nullable_decoder = (config, loc, inner) =>
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeNull(value)) {
    | None => Some([%e generate_parser(config, inner)])
    | Some(_) => None
    }
  )
and generate_array_decoder = (config, loc, inner) =>
  [@metaloc loc]
  [%expr
    value
    |> Js.Json.decodeArray
    |> Js.Option.getExn
    |> Js.Array.map(value => {
         %e
         generate_parser(config, inner)
       })
  ]
and generate_array_decoder_lean = (config, loc, inner) =>
  [@metaloc loc]
  [%expr
    Obj.magic(value)
    |> Js.Array.map(value => {
         %e
         generate_parser(config, inner)
       })
  ]
and generate_custom_decoder = (config, loc, ident, inner) => {
  let fn_expr =
    Ast_helper.(
      Exp.ident({
        loc: Location.none,
        txt: Longident.parse(ident ++ ".parse"),
      })
    );
  [@metaloc loc] [%expr [%e fn_expr]([%e generate_parser(config, inner)])];
}
and generate_object_decoder = (config, loc, name, fields) => {
  let rec do_obj_constructor = () => {
    Ast_406.(
      Ast_helper.(
        Exp.extension((
          {txt: "bs.obj", loc},
          PStr([[%stri [%e do_obj_constructor_lean()]]]),
        ))
      )
    );
  }
  and do_obj_constructor_lean = () => {
    Ast_helper.Exp.record(
      fields
      |> List.map(
           fun
           | Fr_named_field(key, _, inner) => (
               {Location.txt: Longident.parse(key), loc},
               {
                 let%expr value =
                   Js.Dict.unsafeGet(
                     Obj.magic(value),
                     [%e const_str_expr(key)],
                   );

                 %e
                 generate_parser(config, inner);
               },
             )
           | Fr_fragment_spread(key, loc, name, _) => (
               {Location.txt: Longident.parse(key), loc: conv_loc(loc)},
               {
                 let ident =
                   Ast_helper.Exp.ident({
                     loc: conv_loc(loc),
                     txt: Longident.parse(name ++ ".parse"),
                   });
                 %expr
                 [%e ident](Obj.magic(value));
               },
             ),
         ),
      None,
    );
  }
  and obj_constructor = () => {
    [@metaloc loc]
    let%expr value = value |> Js.Json.decodeObject |> Js.Option.getExn;
    %e
    do_obj_constructor();
  }
  and obj_constructor_lean = () =>
    [@metaloc loc]
    {
      do_obj_constructor_lean();
    };

  config.records ? obj_constructor_lean() : obj_constructor();
}
and generate_poly_variant_selection_set = (config, loc, name, fields) => {
  let rec generator_loop =
    fun
    | [(field, inner), ...next] => {
        let variant_decoder =
          Ast_helper.(
            Exp.variant(
              Compat.capitalize_ascii(field),
              Some(generate_parser(config, inner)),
            )
          );
        switch%expr (Js.Dict.get(value, [%e const_str_expr(field)])) {
        | None =>
          %e
          make_error_raiser(
            [%expr
              "Field "
              ++ [%e const_str_expr(field)]
              ++ " on type "
              ++ [%e const_str_expr(name)]
              ++ " is missing"
            ],
          )
        | Some(temp) =>
          switch (Js.Json.decodeNull(temp)) {
          | None =>
            let value = temp;
            %e
            variant_decoder;
          | Some(_) =>
            %e
            generator_loop(next)
          }
        };
      }
    | [] =>
      make_error_raiser(
        [%expr
          "All fields on variant selection set on type "
          ++ [%e const_str_expr(name)]
          ++ " were null"
        ],
      );

  let variant_type =
    Ast_helper.(
      Typ.variant(
        fields
        |> List.map(((name, _)) =>
             Rtag(
               {txt: Compat.capitalize_ascii(name), loc},
               [],
               false,
               [
                 {
                   ptyp_desc: Ptyp_any,
                   ptyp_attributes: [],
                   ptyp_loc: Location.none,
                 },
               ],
             )
           ),
        Closed,
        None,
      )
    );
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeObject(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr
          "Expected type " ++ [%e const_str_expr(name)] ++ " to be an object"
        ],
      )
    | Some(value) => ([%e generator_loop(fields)]: [%t variant_type])
    }
  );
}
and generate_poly_variant_interface = (config, loc, name, base, fragments) => {
  let map_fallback_case = ((type_name, inner)) => {
    open Ast_helper;
    let name_pattern = Pat.any();

    Exp.variant(type_name, Some(generate_parser(config, inner)))
    |> Exp.case(name_pattern);
  };

  let map_case = ((type_name, inner)) => {
    open Ast_helper;
    let name_pattern = Pat.constant(Pconst_string(type_name, None));

    Exp.variant(type_name, Some(generate_parser(config, inner)))
    |> Exp.case(name_pattern);
  };
  let map_case_ty = ((name, _)) =>
    Rtag(
      {txt: name, loc},
      [],
      false,
      [{ptyp_desc: Ptyp_any, ptyp_attributes: [], ptyp_loc: Location.none}],
    );

  let fragment_cases = List.map(map_case, fragments);
  let fallback_case = map_fallback_case(base);
  let fallback_case_ty = map_case_ty(base);

  let fragment_case_tys = List.map(map_case_ty, fragments);
  let interface_ty =
    Ast_helper.(
      Typ.variant([fallback_case_ty, ...fragment_case_tys], Closed, None)
    );
  let typename_matcher =
    Ast_helper.(
      Exp.match(
        [%expr typename],
        List.concat([fragment_cases, [fallback_case]]),
      )
    );
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeObject(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr
          "Expected Interface implementation "
          ++ [%e const_str_expr(name)]
          ++ " to be an object, got "
          ++ Js.Json.stringify(value)
        ],
      )
    | Some(typename_obj) =>
      switch (Js.Dict.get(typename_obj, "__typename")) {
      | None =>
        %e
        make_error_raiser(
          [%expr
            "Interface implementation"
            ++ [%e const_str_expr(name)]
            ++ " is missing the __typename field"
          ],
        )
      | Some(typename) =>
        switch (Js.Json.decodeString(typename)) {
        | None =>
          %e
          make_error_raiser(
            [%expr
              "Interface implementation "
              ++ [%e const_str_expr(name)]
              ++ " has a __typename field that is not a string"
            ],
          )
        | Some(typename) => ([%e typename_matcher]: [%t interface_ty])
        }
      }
    }
  );
}
and generate_poly_variant_union =
    (config, loc, name, fragments, exhaustive_flag) => {
  let fragment_cases =
    Ast_helper.(
      fragments
      |> List.map(((type_name, inner)) => {
           let name_pattern = Pat.constant(Pconst_string(type_name, None));
           Ast_helper.(
             Exp.variant(type_name, Some(generate_parser(config, inner)))
           )
           |> Exp.case(name_pattern);
         })
    );
  let (fallback_case, fallback_case_ty) =
    Ast_helper.(
      switch (exhaustive_flag) {
      | Result_structure.Exhaustive => (
          Exp.case(
            Pat.var({loc: Location.none, txt: "typename"}),
            make_error_raiser(
              [%expr
                "Union "
                ++ [%e const_str_expr(name)]
                ++ " returned unknown type "
                ++ typename
              ],
            ),
          ),
          [],
        )
      | Nonexhaustive => (
          Exp.case(Pat.any(), [%expr `Nonexhaustive]),
          [Rtag({txt: "Nonexhaustive", loc}, [], true, [])],
        )
      }
    );
  let fragment_case_tys =
    fragments
    |> List.map(((name, _)) =>
         Rtag(
           {txt: name, loc},
           [],
           false,
           [
             {
               ptyp_desc: Ptyp_any,
               ptyp_attributes: [],
               ptyp_loc: Location.none,
             },
           ],
         )
       );

  let union_ty =
    Ast_helper.(
      Typ.variant(
        List.concat([fallback_case_ty, fragment_case_tys]),
        Closed,
        None,
      )
    );
  let typename_matcher =
    Ast_helper.(
      Exp.match(
        [%expr typename],
        List.concat([fragment_cases, [fallback_case]]),
      )
    );
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeObject(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr
          "Expected union "
          ++ [%e const_str_expr(name)]
          ++ " to be an object, got "
          ++ Js.Json.stringify(value)
        ],
      )
    | Some(typename_obj) =>
      switch (Js.Dict.get(typename_obj, "__typename")) {
      | None =>
        %e
        make_error_raiser(
          [%expr
            "Union "
            ++ [%e const_str_expr(name)]
            ++ " is missing the __typename field"
          ],
        )
      | Some(typename) =>
        switch (Js.Json.decodeString(typename)) {
        | None =>
          %e
          make_error_raiser(
            [%expr
              "Union "
              ++ [%e const_str_expr(name)]
              ++ " has a __typename field that is not a string"
            ],
          )
        | Some(typename) => ([%e typename_matcher]: [%t union_ty])
        }
      }
    }
  );
};
