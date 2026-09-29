// Page and text style of the UltraScan internal discussion papers
// (after 2dsa_subgrid_coverage_v1):  Computer Modern, numbered sections,
// three-rule tables captioned above, figures captioned below.

#let paper(
  title: none,
  subtitle: none,
  short-title: none,
  date: none,
  abstract: none,
  body,
) = {
  set document(title: title)
  set text(font: "New Computer Modern", size: 10.5pt, lang: "en")
  set par(justify: true, leading: 0.6em, spacing: 0.9em, first-line-indent: (amount: 1.2em, all: false))
  set page(
    paper: "us-letter",
    margin: (x: 1.05in, top: 1in, bottom: 1in),
    header: context {
      if counter(page).get().first() > 1 {
        set text(size: 8.5pt, fill: luma(90))
        grid(
          columns: (1fr, auto),
          [UltraScan internal discussion paper], [#short-title],
        )
      }
    },
    footer: context align(center, text(size: 9pt, counter(page).display())),
  )

  set heading(numbering: "1.1")
  show heading: set block(above: 1.5em, below: 0.9em)
  show heading.where(level: 1): set text(size: 13pt, weight: "bold")
  show heading.where(level: 2): set text(size: 11pt, weight: "bold")
  show heading: it => {
    if it.numbering == none { it } else {
      block(counter(heading).display(it.numbering) + h(1em) + it.body)
    }
  }

  // Tables:  caption above; figures:  caption below
  show figure.where(kind: table): set figure.caption(position: top)
  set figure.caption(separator: [. ])
  show figure.caption: it => {
    set text(size: 9.5pt)
    set par(justify: true, first-line-indent: 0pt)
    block(width: 100%, align(left)[
      #text(weight: "bold")[#it.supplement #context it.counter.display(it.numbering).] #it.body
    ])
  }
  show figure: set block(above: 1.4em, below: 1.4em)
  set table(stroke: none, inset: (x: 6pt, y: 3.2pt))
  show table: set text(size: 9pt)
  set math.equation(numbering: "(1)")
  show link: set text(fill: rgb("#1c5cab"))
  show ref: set text(fill: rgb("#1c5cab"))

  // Title block
  align(center)[
    #text(size: 9pt, fill: luma(90))[UltraScan internal discussion paper #h(0.6em) · #h(0.6em) #date]
    #v(1.6em)
    #text(size: 17pt, weight: "bold")[#title]
    #v(0.5em)
    #text(size: 12pt)[#subtitle]
    #v(1.2em)
    #text(size: 10pt, weight: "bold")[Abstract]
  ]
  pad(x: 2.2em)[
    #set text(size: 9.5pt)
    #set par(first-line-indent: 0pt)
    #abstract
  ]
  v(1em)
  body
}

// Three-rule ("booktabs") table:  header row between the top and middle rules
#let rules-table(columns: auto, align: auto, header: (), groups: (), ..rows) = table(
  columns: columns,
  align: align,
  table.hline(stroke: 0.8pt),
  table.header(..groups, ..header.map(h => text(weight: "regular")[#h])),
  table.hline(stroke: 0.5pt),
  ..rows,
  table.hline(stroke: 0.8pt),
)
