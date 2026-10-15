const tagTypeMap: Record<string, string> = {
    "tag": "general",
    "metadata": "meta",
};

/**
 * Danbooru and Sankaku write OR as "~a ~b", but Gelbooru-style sites need "( a ~ b )".
 * Given "~a ~b" verbatim, rule34 reads each "~tag" as a literal tag that does not exist and
 * returns 0 posts - silently, no error - so a tab searching all three sites can never share
 * one query string. Translating here means "~a ~b" works everywhere.
 *
 * Verified against the live API 2026-08-13: groups are not width-limited here. A 5-wide
 * "( a ~ b ~ c ~ d ~ e )" unions correctly. The real ceiling is Danbooru's max-tags-per-search,
 * which counts each "~tag" separately: 2 for Member, 6 for Gold, unlimited for Platinum.
 */
function translateOrTags(search: string): string {
    const matches = search.match(/(^|\s)~\S+/g);
    if (!matches) {
        return search;
    }
    const terms = matches.map((m) => m.replace(/^\s+/, "").substring(1));
    const rest = search.replace(/(^|\s)~\S+/g, " ").replace(/\s+/g, " ").trim();
    // A lone "~tag" means just that tag on Danbooru, so drop the marker rather than
    // emitting a one-element OR group.
    const group = terms.length > 1 ? "( " + terms.join(" ~ ") + " )" : terms[0];
    return rest ? group + " " + rest : group;
}

function completeImage(img: IImage): IImage {
    // Only set `author` when the API actually returned an owner. The XML and HTML APIs
    // (rule34.xxx) have no `owner` field, and assigning `undefined` makes the C++ host take a
    // slow undefined-value path costing ~200ms per image (javascript-api.cpp:241).
    if ((img as any).owner !== undefined) {
        img.author = (img as any).owner;
    }

    if ("tag_info" in img) {
        const tagInfo: Array<{ tag: string; type: string; count: number }> = img["tag_info"] as any;
        img.tags = tagInfo.map((tag) => ({
            name: tag.tag,
            type: tagTypeMap[tag.type] || tag.type,
            count: tag.count,
        }));
    }

    if ((!img.file_url || img.file_url.length < 5) && img.preview_url) {
        img.file_url = img.preview_url
            .replace("/thumbnails/", "/images/")
            .replace("/thumbnail_", "/");
    }

    return img;
}

export const source: ISource = {
    name: "Gelbooru (0.2)",
    modifiers: ["rating:general", "rating:safe", "rating:questionable", "rating:explicit", "user:", "fav:", "fastfav:", "md5:", "source:", "id:", "width:", "height:", "score:", "mpixels:", "filesize:", "date:", "gentags:", "arttags:", "chartags:", "copytags:", "approver:", "parent:", "sub:", "order:id", "order:id_desc", "order:score", "order:score_asc", "order:mpixels", "order:mpixels_asc", "order:filesize", "order:landscape", "order:portrait", "order:favcount", "order:rank", "parent:none", "unlocked:rating", "sort:updated", "sort:id", "sort:score", "sort:rating", "sort:user", "sort:height", "sort:width", "sort:parent", "sort:source", "sort:updated"],
    tagFormat: {
        case: "lower",
        wordSeparator: "_",
    },
    searchFormat: {
        and: " ",
    },
    auth: {
        url:  {
            type: "url",
            fields: [
                {
                    id: "userId",
                    key: "user_id",
                },
                {
                    id: "apiKey",
                    key: "api_key",
                },
            ],
        },
        session: {
            type: "post",
            url: "/index.php?page=account&s=login&code=00",
            fields: [
                {
                    id: "pseudo",
                    key: "user",
                },
                {
                    id: "password",
                    key: "pass",
                    type: "password",
                },
            ],
            check: {
                type: "cookie",
                key: "user_id",
            },
        },
    },
    apis: {
        xml: {
            name: "XML",
            auth: [],
            maxLimit: 100,
            search: {
                url: (query: ISearchQuery, opts: IUrlOptions): string | IError => {
                    const page: number = query.page - 1;
                    const search: string = translateOrTags(query.search.replace(/(^| )order:/gi, "$1sort:"));
                    const fav = search.match(/(?:^| )fav:(\d+)(?:$| )/);
                    if (fav) {
                        return { error: "XML API cannot search favorites" };
                    }
                    return "/index.php?page=dapi&s=post&q=index&limit=" + opts.limit + "&pid=" + page + "&tags=" + encodeURIComponent(search);
                },
                parse: (src: string): IParsedSearch | IError => {
                    const parsed = Grabber.parseXML(src);
                    if (!parsed) {
                        return { error: "Invalid XML response" };
                    }

                    // Handle error messages
                    if (parsed?.["response"]?.["@attributes"]?.["success"] === "false") {
                        return { error: parsed["response"]["@attributes"]["reason"] };
                    }
                    if ("error" in parsed) {
                        const error = parsed["error"];
                        return { error: typeof error === "string" ? error : String(error["#text"] || "Unknown API error") };
                    }

                    // Validate the shape of the response
                    if (!parsed?.["posts"]) {
                        return { error: "Invalid XML response (no posts found)" };
                    }

                    const data = Grabber.makeArray(parsed.posts.post);
                    const images: IImage[] = [];
                    for (const image of data) {
                        if (image && "id" in image) {
                            images.push(completeImage(Grabber.typedXML(image)));
                        } else if (image && "@attributes" in image) {
                            images.push(completeImage(image["@attributes"]));
                        }
                    }

                    return {
                        images,
                        imageCount: parsed.posts["@attributes"]?.["count"],
                    };
                },
            },
        },
        json: {
            name: "JSON",
            auth: [],
            maxLimit: 100,
            search: {
                url: (query: ISearchQuery, opts: IUrlOptions): string | IError => {
                    const page: number = query.page - 1;
                    const search: string = translateOrTags(query.search.replace(/(^| )order:/gi, "$1sort:"));
                    const fav = search.match(/(?:^| )fav:(\d+)(?:$| )/);
                    if (fav) {
                        return { error: "JSON API cannot search favorites" };
                    }
                    return "/index.php?page=dapi&s=post&q=index&limit=" + opts.limit + "&pid=" + page + "&tags=" + encodeURIComponent(search) + "&json=1";
                },
                parse: (src: string): IParsedSearch | IError => {
                    let parsed = JSON.parse(src);

                    // Handle error messages
                    if (typeof parsed === "string") {
                        return { error: parsed };
                    }

                    // Handle the old format
                    if (Array.isArray(parsed)) {
                        parsed = {
                            "@attributes": {},
                            "post": parsed,
                        };
                    }

                    const images: IImage[] = [];
                    for (const image of parsed["post"]) {
                        images.push(completeImage(image));
                    }

                    return {
                        images,
                        imageCount: parsed["@attributes"]["count"],
                    };
                },
            },
            details: {
                url: (id: string, md5: string, opts: IUrlDetailsOptions): string => {
                    return "/index.php?page=dapi&s=post&q=index&id=" + id + "&fields=tag_info&json=1";
                },
                parse: (src: string): IParsedDetails => {
                    const parsed = JSON.parse(src);
                    const post = "post" in parsed ? parsed["post"] : parsed;
                    const image = (Array.isArray(post) ? post[0] : post) || {};
                    // Local: this source does not declare `details.fullResults`, so the host reads
                    // discrete properties here rather than an image object (javascript-api.cpp:636).
                    // Returning completeImage() directly drops imageUrl silently - keep the mapping.
                    const img = completeImage(image);
                    return { tags: img.tags || [], imageUrl: img.file_url, source: (img as any).source };
                },
            },
        },
        html: {
            name: "Regex",
            auth: [],
            forcedLimit: 42,
            search: {
                url: (query: ISearchQuery, opts: IUrlOptions, previous: IPreviousSearch | undefined): string | IError => {
                    try {
                        const baseUrl = opts.baseUrl.replace("//api.", "//");
                        const search: string = translateOrTags(query.search.replace(/(^| )order:/gi, "$1sort:"));
                        const fav = search.match(/(?:^| )fav:(\d+)(?:$| )/);
                        if (fav) {
                            const pagePart = Grabber.pageUrl(query.page, previous, 20000, "&pid={page}", "&pid={page}", " id:<{min}&p=1", (p: number) => (p - 1) * 50);
                            return baseUrl + "/index.php?page=favorites&s=view&id=" + fav[1] + pagePart;
                        } else {
                            const pagePart = Grabber.pageUrl(query.page, previous, 20000, "&pid={page}", "&pid={page}", " id:<{min}&p=1", (p: number) => (p - 1) * 42);
                            return baseUrl + "/index.php?page=post&s=list&tags=" + encodeURIComponent(search) + pagePart;
                        }
                    } catch (e: any) {
                        return { error: e.message };
                    }
                },
                parse: (src: string): IParsedSearch | IError => {
                    if (src.indexOf("Unable to search this deep") !== -1) {
                        return { error: "Page too far" };
                    }
                    const pageCountRaw = Grabber.regexMatch('<a href="[^"]+pid=(?<page>\\d+)[^"]*"[^>]*>[^<]+</a>\\s*(?:<b>(?<page_2>\\d+)</b>\\s*)?(?:</div>|<br ?/>)|<a href="[^"]+pid=(?<page_3>\\d+)[^"]*"[^>]*>(?:&raquo;|>>)</a>', src);
                    const pageCount = pageCountRaw && pageCountRaw["page"];
                    const images = Grabber.regexToImages('(?:<span[^>]*(?:\\sid="?\\w(?<id>\\d+)"?)?>\\s*)?<a[^>]*(?:\\sid="?\\w(?<id_2>\\d+)"?)[^>]*>\\s*<img [^>]*(?:src|data-original)="(?<preview_url>[^"]+/thumbnail_(?<md5>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags>[^"]+)"[^>]*/?>\\s*</a>|<img\\s+class="preview"\\s+src="(?<preview_url_2>[^"]+/thumbnail_(?<md5_2>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags_2>[^"]+)"[^>]*/?>', src);
                    for (const img of images) {
                        const json = src.match(new RegExp("posts\\[" + img.id + "\\]\\s*=\\s*({[\\s*\\S]+?})"))?.[1];
                        if (json) {
                            img.rating = json.match(/["']?rating["']?\s*:\s*["']([^"']+)["']/)?.[1].toLowerCase();
                            img.score  = json.match(/["']?score["']?\s*:\s*["'](\d+)["']/)?.[1];
                            img.author = json.match(/["']?user["']?\s*:\s*["']([^'"]+)["']/)?.[1];
                        }
                    }
                    return {
                        images: images.map(completeImage),
                        tags: Grabber.regexToTags('<li class="tag-type-(?<type>[^"]+)">(?:[^<]*<a[^>]*>[^<]*</a>)*[^<]*<a[^>]*>\\s*(?<name>[^<]*?)\\s*</a>[^<]*<span[^>]*>\\s*(?<count>\\d+)\\s*</span>', src),
                        pageCount: pageCount ? parseInt(pageCount, 10) / 42 + 1 : undefined,
                    };
                },
            },
            details: {
                url: (id: string, md5: string, opts: IUrlDetailsOptions): string => {
                    // Local: rule34 exposes typed tags (artist/copyright/character) via the JSON
                    // details endpoint, which avoids scraping the HTML page. This cut
                    // "Loading image details" calls from ~17k/session to near zero.
                    if (opts.baseUrl && opts.baseUrl.indexOf("rule34.xxx") !== -1) {
                        return "/index.php?page=dapi&s=post&q=index&id=" + id + "&fields=tag_info&json=1";
                    }
                    const baseUrl = opts.baseUrl.replace("//api.", "//");
                    return baseUrl + "/index.php?page=post&s=view&id=" + id;
                },
                parse: (src: string): IParsedDetails => {
                    // Local: paired with the JSON details URL above. When that shortcut is used
                    // the body is JSON, not HTML, so detect it before falling through to the
                    // HTML scrapers. Restoring the URL without this would break rule34 details.
                    const head = src.replace(/^\s+/, "").charAt(0);
                    if (head === "[" || head === "{") {
                        const data = JSON.parse(src);
                        const post = (Array.isArray(data) ? data[0] : data) || {};
                        const img = completeImage(post);
                        return { tags: img.tags || [], imageUrl: img.file_url, source: (img as any).source };
                    }
                    return {
                        tags: Grabber.regexToTags('<li class="tag-type-(?<type>[^"]+)">(?:[^<]*(?:<span[^>]*>[^<]*)?<a[^>]*>[^<]*</a>(?:[^<]*</span>)?)*[^<]*<a[^>]*>(?<name>[^<]*)</a>[^<]*<span[^>]*>(?<count>\\d+)</span>[^<]*</li>', src),
                        imageUrl: Grabber.regexToConst("url", '<img[^>]+src="([^"]+)"[^>]+onclick="Note\\.toggle\\(\\);"[^>]*/>', src),
                        createdAt: src.match(/>Posted:\s*([^<]+)</i)?.[1],
                        source: src.match(/>Source:\s*<a href="([^"]+)"/i)?.[1],
                    };
                },
            },
            endpoints: {
                pool_list: {
                    name: "Pools",
                    input: {},
                    url: (query: Record<never, string>, opts: IUrlOptions): string => {
                        const pid = (opts.page - 1) * 25;
                        return "/index.php?page=pool&s=list&pid=" + String(pid);
                    },
                    parse: (src: string): IParsedSearch => {
                        const html = Grabber.parseHTML(src);

                        const images: IImage[] = [];

                        const rows = html.find("table tr");
                        for (const row of rows) {
                            const parts = row.find("td");
                            const link = parts[1].find("a")[0];
                            const id = link.attr("href").match(/id=(\d+)/)[1];

                            images.push({
                                id,
                                name: link.innerText(),
                                type: "gallery",
                                gallery_count: parts[2].innerText().match(/(\d+)\s+Images/)[1],
                                details_endpoint: {
                                    endpoint: "pool_details",
                                    input: { id },
                                },
                            })
                        }

                        return { images };
                    },
                },
                pool_details: {
                    input: {
                        id: {
                            type: "input",
                        },
                    },
                    url: (query: Record<"id", number>): string => {
                        return "/index.php?page=pool&s=show&id=" + String(query.id);
                    },
                    parse: (src: string): IParsedGallery => {
                        const images = Grabber.regexToImages('<span[^>]*(?: id="?\\w(?<id>\\d+)"?)?>\\s*<a[^>]*(?: id="?\\w(?<id_2>\\d+)"?)[^>]*>\\s*<img [^>]*(?:src|data-original)="(?<preview_url>[^"]+/thumbnail_(?<md5>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags>[^"]+)"[^>]*/?>\\s*</a>|<img\\s+class="preview"\\s+src="(?<preview_url_2>[^"]+/thumbnail_(?<md5_2>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags_2>[^"]+)"[^>]*/?>', src);
                        return {
                            images: images.map(completeImage),
                        };
                    },
                },
            },
            tagTypes: false,
            tags: {
                url: (query: ITagsQuery, opts: IUrlOptions): string => {
                    const baseUrl = opts.baseUrl.replace("//api.", "//");
                    const sorts = { count: "desc", date: "asc", name: "asc" };
                    const orderBys = { count: "index_count", date: "updated", name: "tag" };
                    const page: number = (query.page - 1) * 50;
                    return baseUrl + "/index.php?page=tags&s=list&pid=" + page + "&sort=" + sorts[query.order] + "&order_by=" + orderBys[query.order];
                },
                parse: (src: string): IParsedTags => {
                    return {
                        tags: Grabber.regexToTags('(?:<td>(?<count_2>\\d+)</td>\\s*)?<td><span class="tag-type-(?<type>[^"]+)"><a[^>]+>(?<name>.+?)</a></span>(?:\\s*<span class="tag-count">(?<count>\\d+)</span>)?</td>', src),
                    };
                },
            },
            check: {
                url: (): string => {
                    return "/";
                },
                parse: (src: string): boolean => {
                    return src.search(/Running Gelbooru(?: Beta)? 0\.2/) !== -1
                        || src.search(/Running <a[^>]*>Gelbooru<\/a>(?: Beta)? 0\.2/) !== -1;
                },
            },
        },
    },
};
